// WaylandColorFeedback.cpp — 读合成器给本窗口表面的 preferred 色彩描述，
// 并按 mpv 的语义声明 HDR10（PQ/BT.2020 + luminances）。
//
// 架构（为什么是这样）：
//   · surface feedback 必须在 **Qt 自己的 wayland 连接**上取（wl_surface 是
//     连接私有的）→ 全局用 QtWaylandClient 的 QWaylandClientExtensionTemplate
//     绑定（与 WaylandIdleInhibit.cpp 同一模式），协议报文用**我们自己生成的**
//     wayland-scanner 代码（`color-management-v1-client-protocol.h`）。
//   · ⚠ 不能用 Qt 的 `QtWayland::wp_color_management_*` 便利类：
//     `ColorManagementFeedback`/`ImageDescriptionInfo`/`ColorManager` 都无
//     export 宏（连 staticMetaObject 都拿不到，dlopen 报 undefined symbol）；
//     而 Qt 导出的 `QtWayland::` 封装又依赖 Qt 内部生成的 `*_interface` 符号，
//     那些同样没导出（实测 `undefined symbol: wp_image_description_v1_interface`）
//     → 只能自己生成协议代码。
//   · ⚠ 初始 preferred 必须**主动 get_preferred()**（协议只在变化时发事件）。
//   · HDR10 声明必须带 **luminances(0,10000,203)**（照抄 mpv
//     wayland_common.c 的 PQ 分支）：实测缺 luminances 时合成器映射明显更暗
//     （同帧亮度 14.0 vs mpv 原生 23.1）。
//
// 失败一律返回负值 → 调用方退 SDR（探测不到就不启用 HDR，不假设）。

#include "WaylandColorFeedback.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>
#include <QWindow>

#include <QtGui/qpa/qplatformwindow_p.h>
#include <QtWaylandClient/qwaylandclientextension.h>

#include "color-management-v1-client-protocol.h"

#define PL_SDR_WHITE_INT 203   /* mpv 的 PQ 参考白（PL_COLOR_SDR_WHITE） */

namespace {

constexpr double SRGB_AREA = 0.1120;
constexpr double WIDE_GAMUT_FACTOR = 1.15;

/// 基色三角面积（xy）：sRGB/BT.709≈0.1120、P3≈0.1521、BT.2020≈0.2109。
/// 用面积而非逐基色枚举：面板无关、无需维护标准值表、对微小差异鲁棒。
double gamutArea(const double *prim)
{
    const double rx = prim[0], ry = prim[1];
    const double gx = prim[2], gy = prim[3];
    const double bx = prim[4], by = prim[5];
    return 0.5 * ((gx - rx) * (by - ry) - (bx - rx) * (gy - ry));
}

QMutex g_lock;
struct vsr_color_pref g_cur {};
uint64_t g_gen = 0;
char g_summary[256] = {0};

QWindow *g_window = nullptr;
::wl_surface *g_surface = nullptr;
::wp_color_manager_v1 *g_cm = nullptr;
::wp_color_management_surface_feedback_v1 *g_feedback = nullptr;
::wp_color_management_surface_v1 *g_csurface = nullptr;
::wp_image_description_v1 *g_pending_desc = nullptr;
::wp_image_description_info_v1 *g_pending_info = nullptr;
struct vsr_color_pref *g_pending = nullptr;   /* info 解析暂存（事件线程） */
void (*g_change_cb)(void *) = nullptr;
void *g_change_user = nullptr;
int g_cm_version = 0;
unsigned char g_tf_supported[64] = {0};
unsigned char g_prim_supported[64] = {0};
unsigned char g_feat_parametric = 0, g_feat_set_luminances = 0, g_feat_scrgb = 0;
unsigned char g_feat_mastering = 0;   /* feature 5：set_mastering_display_primaries
                                       * （协议规定 set_mastering_luminance 也由它门控）*/

void publish(struct vsr_color_pref p)
{
    QMutexLocker lk(&g_lock);
    p.generation = ++g_gen;
    g_cur = p;
}

void notifyChanged()
{
    if (!g_change_cb || !g_change_user)
        return;
    QWindow *w = g_window;
    void (*cb)(void *) = g_change_cb;
    void *user = g_change_user;
    if (w)
        QMetaObject::invokeMethod(w, [cb, user] { cb(user); }, Qt::QueuedConnection);
    else
        cb(user);
}

// ── wp_image_description_info_v1：解析 preferred 描述 ──────────────────
void info_icc_file(void *d, struct wp_image_description_info_v1 *i, int32_t fd, uint32_t size)
{
    (void)i; (void)size;
    if (fd >= 0) close(fd);
    ((struct vsr_color_pref *)d)->icc_only = 1;   /* 不解析 ICC（不引 lcms） */
}

void info_primaries(void *d, struct wp_image_description_info_v1 *i,
                    int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                    int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
    (void)i;
    struct vsr_color_pref *p = (struct vsr_color_pref *)d;
    const double f = 1000000.0;
    p->prim[0] = rx / f; p->prim[1] = ry / f;
    p->prim[2] = gx / f; p->prim[3] = gy / f;
    p->prim[4] = bx / f; p->prim[5] = by / f;
    p->prim[6] = wx / f; p->prim[7] = wy / f;
    p->have_primaries = 1;
}

void info_primaries_named(void *d, struct wp_image_description_info_v1 *i, uint32_t v)
{
    (void)i;
    struct vsr_color_pref *p = (struct vsr_color_pref *)d;
    p->named_primaries = v;
    p->have_named_primaries = 1;
}

void info_tf_power(void *d, struct wp_image_description_info_v1 *i, uint32_t eexp)
{
    (void)d; (void)i; (void)eexp;
}

void info_tf_named(void *d, struct wp_image_description_info_v1 *i, uint32_t v)
{
    (void)i;
    struct vsr_color_pref *p = (struct vsr_color_pref *)d;
    p->tf = v; p->have_tf = 1;
}

void info_luminances(void *d, struct wp_image_description_info_v1 *i,
                     uint32_t min_lum, uint32_t max_lum, uint32_t ref_lum)
{
    (void)i;
    struct vsr_color_pref *p = (struct vsr_color_pref *)d;
    p->min_lum = min_lum / 10000.0;
    p->max_lum = max_lum;
    p->ref_lum = ref_lum;
    p->have_luminances = 1;
}

void info_target_primaries(void *d, struct wp_image_description_info_v1 *i,
                           int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                           int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
    (void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy;
    (void)bx; (void)by; (void)wx; (void)wy;
}

void info_target_luminance(void *d, struct wp_image_description_info_v1 *i,
                           uint32_t min_lum, uint32_t max_lum)
{
    (void)i;
    struct vsr_color_pref *p = (struct vsr_color_pref *)d;
    p->target_min_lum = min_lum / 10000.0;
    p->target_max_lum = max_lum;
    p->have_target = 1;
}

void info_target_max_cll(void *d, struct wp_image_description_info_v1 *i, uint32_t v)
{
    (void)i; ((struct vsr_color_pref *)d)->max_cll = v;
}

void info_target_max_fall(void *d, struct wp_image_description_info_v1 *i, uint32_t v)
{
    (void)i; ((struct vsr_color_pref *)d)->max_fall = v;
}

void info_done(void *d, struct wp_image_description_info_v1 *i)
{
    wp_image_description_info_v1_destroy(i);
    g_pending_info = nullptr;
    struct vsr_color_pref p = *(struct vsr_color_pref *)d;
    delete (struct vsr_color_pref *)d;
    g_pending = nullptr;

    p.valid = 1;
    p.gamut_area = p.have_primaries ? gamutArea(p.prim) : 0.0;
    const bool pq = p.have_tf &&
        (p.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ||
         p.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG);
    const bool wide = p.have_primaries && p.gamut_area > SRGB_AREA * WIDE_GAMUT_FACTOR;
    p.hdr_volume = pq || wide;
    uint64_t gen;
    publish(p);
    { QMutexLocker lk(&g_lock); gen = g_cur.generation; }
    fprintf(stderr, "vsr-color-fb: preferred gen=%llu tf=%u named_prim=%u "
                    "lum(min=%.4f max=%.1f ref=%.1f target_max=%.1f) "
                    "gamut_area=%.4f g=(%.4f,%.4f) → %s\n",
            (unsigned long long)gen, p.tf, p.named_primaries,
            p.min_lum, p.max_lum, p.ref_lum, p.target_max_lum,
            p.gamut_area, p.prim[2], p.prim[3],
            p.hdr_volume ? "HDR/wide volume" : "SDR volume");
    notifyChanged();
}

/* 注意：C++ 的指定初始化器必须与结构体声明同序（生成头里 done 在第一位） */
static const struct wp_image_description_info_v1_listener info_listener = {
    .done = info_done,
    .icc_file = info_icc_file,
    .primaries = info_primaries,
    .primaries_named = info_primaries_named,
    .tf_power = info_tf_power,
    .tf_named = info_tf_named,
    .luminances = info_luminances,
    .target_primaries = info_target_primaries,
    .target_luminance = info_target_luminance,
    .target_max_cll = info_target_max_cll,
    .target_max_fall = info_target_max_fall,
};

/// 取一次当前 preferred 描述（协议只在变化时发事件 → 初始值必须主动取）。
void query_preferred()
{
    if (!g_feedback)
        return;
    if (g_pending_info) {   /* 丢弃未完成的 pending（合成器会补发变化） */
        wp_image_description_info_v1_destroy(g_pending_info);
        g_pending_info = nullptr;
    }
    if (g_pending_desc) {
        wp_image_description_v1_destroy(g_pending_desc);
        g_pending_desc = nullptr;
    }
    delete g_pending;
    g_pending = nullptr;

    g_pending_desc = wp_color_management_surface_feedback_v1_get_preferred(g_feedback);
    if (!g_pending_desc)
        return;
    g_pending_info = wp_image_description_v1_get_information(g_pending_desc);
    if (!g_pending_info)
        return;
    g_pending = new struct vsr_color_pref();
    memset(g_pending, 0, sizeof *g_pending);
    wp_image_description_info_v1_add_listener(g_pending_info, &info_listener, g_pending);
}

void feedback_preferred_changed(void *d, struct wp_color_management_surface_feedback_v1 *f,
                                uint32_t identity)
{
    (void)d; (void)f; (void)identity;
    query_preferred();
}

#ifdef WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION
void feedback_preferred_changed2(void *d, struct wp_color_management_surface_feedback_v1 *f,
                                 uint32_t hi, uint32_t lo)
{
    (void)d; (void)f; (void)hi; (void)lo;
    query_preferred();
}
#endif

static const struct wp_color_management_surface_feedback_v1_listener feedback_listener = {
    .preferred_changed = feedback_preferred_changed,
#ifdef WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION
    .preferred_changed2 = feedback_preferred_changed2,
#endif
};

// ── wp_color_manager_v1 能力（诊断 + 声明前预检）───────────────────────
void cm_supported_intent(void *d, struct wp_color_manager_v1 *m, uint32_t i)
{
    (void)d; (void)m; (void)i;
}
void cm_supported_feature(void *d, struct wp_color_manager_v1 *m, uint32_t f)
{
    (void)d; (void)m;
    if (f == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) g_feat_parametric = 1;
    else if (f == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) g_feat_set_luminances = 1;
    else if (f == WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) g_feat_mastering = 1;
    else if (f == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB) g_feat_scrgb = 1;
}
void cm_supported_tf_named(void *d, struct wp_color_manager_v1 *m, uint32_t v)
{
    (void)d; (void)m;
    if (v < sizeof g_tf_supported) g_tf_supported[v] = 1;
}
void cm_supported_primaries_named(void *d, struct wp_color_manager_v1 *m, uint32_t v)
{
    (void)d; (void)m;
    if (v < sizeof g_prim_supported) g_prim_supported[v] = 1;
}
void cm_done(void *d, struct wp_color_manager_v1 *m) { (void)d; (void)m; }

static const struct wp_color_manager_v1_listener cm_listener = {
    .supported_intent = cm_supported_intent,
    .supported_feature = cm_supported_feature,
    .supported_tf_named = cm_supported_tf_named,
    .supported_primaries_named = cm_supported_primaries_named,
    .done = cm_done,
};

/// 订阅本窗口表面的 preferred 描述（需要 g_cm + g_surface 都就绪）。
/// 可在事件线程（全局到齐时）或主线程（重试）调用。
int attach_feedback()
{
    if (g_feedback)
        return 0;
    if (!g_cm || !g_surface)
        return -2;
    g_feedback = wp_color_manager_v1_get_surface_feedback(g_cm, g_surface);
    if (!g_feedback)
        return -3;
    wp_color_management_surface_feedback_v1_add_listener(
        g_feedback, &feedback_listener, nullptr);
    query_preferred();            /* 初始值必须主动取 */
    return 0;
}

/// Qt registry 绑定入口（事件线程回调 init()）——WaylandIdleInhibit.cpp 同款。
class ColorManagerExt : public QWaylandClientExtensionTemplate<ColorManagerExt>
{
public:
    ColorManagerExt() : QWaylandClientExtensionTemplate(3) {}

    static const wl_interface *interface() { return &wp_color_manager_v1_interface; }

    void init(wl_registry *registry, int id, int version)
    {
        if (m_obj)
            return;
        g_cm_version = version;
        m_obj = static_cast<struct wp_color_manager_v1 *>(
            wl_registry_bind(registry, id, &wp_color_manager_v1_interface,
                             static_cast<uint32_t>(version)));
        g_cm = m_obj;
        wp_color_manager_v1_add_listener(m_obj, &cm_listener, nullptr);
        fprintf(stderr, "vsr-color-fb: wp_color_manager_v1 bound (v%d)\n", version);
        // 全局可能在首次 vsr_color_fb_init() 之后才绑上 → 这里补做订阅，
        // 否则要等下一次 init 调用（可能很久，画面已经按 SDR 起播）。
        if (attach_feedback() == 0)
            fprintf(stderr, "vsr-color-fb: surface feedback 已订阅（全局到齐后补做）\n");
    }

    struct wp_color_manager_v1 *obj() const { return m_obj; }

private:
    struct wp_color_manager_v1 *m_obj = nullptr;
};

ColorManagerExt *manager()
{
    static ColorManagerExt *s = nullptr;
    if (!s)
        s = new ColorManagerExt();
    return s;
}

}  // namespace

extern "C" int vsr_color_fb_init(void *qwindow)
{
    if (!qwindow)
        return -1;
    auto *win = static_cast<QWindow *>(qwindow);
    auto *iface = win->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!iface)
        return -2;                    /* 非 Wayland 窗口 */
    ::wl_surface *surf = iface->surface();
    if (!surf)
        return -2;                    /* 平台窗口还没建（show 之前）→ 重试 */

    /* 先记下窗口/表面：全局稍后才绑上时，由 ColorManagerExt::init() 补订阅。 */
    g_window = win;
    g_surface = surf;

    ColorManagerExt *mgr = manager();
    if (!mgr->isActive() || !mgr->obj())
        return -2;                    /* 全局还没绑定完：稍后重试/由 init() 补做 */

    const int arc = attach_feedback();
    if (arc != 0)
        return arc;

    snprintf(g_summary, sizeof g_summary,
             "wp_color_manager_v1 v%d features{parametric=%d set_luminances=%d "
             "set_mastering=%d scrgb=%d} "
             "tf{bt1886=%d gamma22=%d ext_linear=%d pq=%d hlg=%d}",
             g_cm_version, g_feat_parametric, g_feat_set_luminances, g_feat_mastering,
             g_feat_scrgb,
             g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886] ? 1 : 0,
             g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22] ? 1 : 0,
             g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR] ? 1 : 0,
             g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ] ? 1 : 0,
             g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG] ? 1 : 0);
    return 0;
}

extern "C" int vsr_color_fb_get(struct vsr_color_pref *out)
{
    if (!out)
        return -1;
    QMutexLocker lk(&g_lock);
    *out = g_cur;
    return 0;
}

/// 合法性过滤（照 mpv `cm_metadata_sanitize()`）：返回 false = 元数据整体不可用。
/// - min_mastering >= max_mastering → 整体丢弃；
/// - v1 合成器额外要求 max_cll/max_fall 落在 (min_mastering, max_mastering] 内；
/// - 两者都设时 max_fall <= max_cll，否则两者清零（协议 invalid_luminance 约束）。
static bool sanitize_meta(const struct vsr_hdr_meta *in, struct vsr_hdr_meta *out)
{
    *out = *in;
    if (!in->valid || out->min_mastering >= out->max_mastering)
        return false;
    if (g_cm_version == 1) {
        if (out->max_cll &&
            (out->max_cll <= out->min_mastering || out->max_cll > out->max_mastering))
            out->max_cll = 0;
        if (out->max_fall &&
            (out->max_fall <= out->min_mastering || out->max_fall > out->max_mastering))
            out->max_fall = 0;
    }
    if (out->max_cll && out->max_fall && out->max_fall > out->max_cll) {
        out->max_cll = 0;
        out->max_fall = 0;
    }
    return true;
}

/// 按 mpv 语义声明 HDR10：primaries=bt2020、tf=st2084_pq、luminances(0,10000,203)
/// + （有内容元数据时）mastering luminance 与 MaxCLL/MaxFALL。
extern "C" int vsr_color_fb_declare_pq(void *qwindow, const struct vsr_hdr_meta *meta)
{
    QWindow *win = qwindow ? static_cast<QWindow *>(qwindow) : g_window;
    if (!win || !g_cm)
        return -1;
    auto *iface = win->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!iface || !iface->surface())
        return -2;
    if (!g_feat_parametric ||
        !g_tf_supported[WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ] ||
        !g_prim_supported[WP_COLOR_MANAGER_V1_PRIMARIES_BT2020]) {
        fprintf(stderr, "vsr-color-fb: 合成器不支持 parametric/PQ/bt2020 声明\n");
        return -3;
    }

    struct wp_image_description_creator_params_v1 *creator =
        wp_color_manager_v1_create_parametric_creator(g_cm);
    if (!creator)
        return -1;
    wp_image_description_creator_params_v1_set_primaries_named(
        creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_tf_named(
        creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    /* mpv wayland_common.c 的 PQ 分支：min=0、max=10000（PQ 名义）、ref=203。
     * ⚠ 协议规定 PQ 下 max_lum 参数被忽略（恒取 min+10000）——内容是"主色体积"
     * 而非内容峰值，内容峰值靠下面的 mastering/CLL 表达。 */
    wp_image_description_creator_params_v1_set_luminances(
        creator, 0, 10000, PL_SDR_WHITE_INT);

    struct vsr_hdr_meta m {};
    const bool have_meta = meta && sanitize_meta(meta, &m);
    if (have_meta) {
        /* 门控与 mpv 一致：mastering 需要 feature set_mastering_display_primaries；
         * mastering 基色（ST 2086）暂不下发（mpv 属性面没暴露，且本应用容器基色
         * 已是 BT.2020，KWin 缺省即按容器基色处理）。 */
        if (g_feat_mastering) {
            wp_image_description_creator_params_v1_set_mastering_luminance(
                creator, (uint32_t)llrint(m.min_mastering * 10000.0), (uint32_t)llrint(m.max_mastering));
        }
        wp_image_description_creator_params_v1_set_max_cll(creator, (uint32_t)llrint(m.max_cll));
        wp_image_description_creator_params_v1_set_max_fall(creator, (uint32_t)llrint(m.max_fall));
    }

    struct wp_image_description_v1 *desc =
        wp_image_description_creator_params_v1_create(creator);
    if (!desc)
        return -1;

    if (!g_csurface)
        g_csurface = wp_color_manager_v1_get_surface(g_cm, iface->surface());
    if (!g_csurface) {
        wp_image_description_v1_destroy(desc);
        return -1;
    }
    wp_color_management_surface_v1_set_image_description(g_csurface, desc, 0);
    if (have_meta) {
        fprintf(stderr, "vsr-color-fb: 已声明 HDR10（bt2020 + st2084_pq + "
                        "luminances 0/10000/203 + mastering %.4f-%.0f nit + "
                        "max_cll %.0f / max_fall %.0f）\n",
                m.min_mastering, m.max_mastering, m.max_cll, m.max_fall);
    } else {
        fprintf(stderr, "vsr-color-fb: 已声明 HDR10（bt2020 + st2084_pq + "
                        "luminances 0/10000/203，mpv 语义；无内容静态元数据）\n");
    }
    return 0;
}

/// 撤下自定义声明 → 合成器回到默认/首选解释（HDR10 → SDR 回落用）。
extern "C" int vsr_color_fb_unset(void *qwindow)
{
    (void)qwindow;
    if (!g_csurface)
        return 0;    /* 从没声明过：本来就是默认 */
    wp_color_management_surface_v1_unset_image_description(g_csurface);
    fprintf(stderr, "vsr-color-fb: 已撤下自定义色彩描述 → 合成器默认\n");
    return 0;
}

extern "C" const char *vsr_color_fb_summary(void) { return g_summary; }

extern "C" void vsr_color_fb_set_change_callback(void (*cb)(void *), void *user)
{
    QMutexLocker lk(&g_lock);
    g_change_cb = cb;
    g_change_user = user;
}

extern "C" void vsr_color_fb_shutdown(void)
{
    if (g_pending_info) { wp_image_description_info_v1_destroy(g_pending_info); g_pending_info = nullptr; }
    if (g_pending_desc) { wp_image_description_v1_destroy(g_pending_desc); g_pending_desc = nullptr; }
    delete g_pending; g_pending = nullptr;
    if (g_csurface) { wp_color_management_surface_v1_destroy(g_csurface); g_csurface = nullptr; }
    if (g_feedback) { wp_color_management_surface_feedback_v1_destroy(g_feedback); g_feedback = nullptr; }
    g_window = nullptr;
    g_surface = nullptr;
    QMutexLocker lk(&g_lock);
    memset(&g_cur, 0, sizeof g_cur);
}
