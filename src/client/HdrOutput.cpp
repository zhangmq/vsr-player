// HdrOutput.cpp — HDR 上屏：能力探测 + 策略决策 + 运行期切换。
//
// 设计/证据见 docs/hdr-pipeline-investigation.md §12–§16：
//   · 显示能力只能问合成器（surface preferred 描述）：Qt 的 hdrInfo() 是
//     硬编码默认值，isFormatSupported() 只能回答"合成器整体支持"。
//   · HDR 目标色彩空间**必须由宿主显式声明**：generic libmpv 渲染路径会丢掉
//     backend 填的 ra_fbo.color_space（见 libmpv_vk.c 注释），所以走
//     `target-trc/prim`（MpvController::setHdrTarget）+ mpv 语义的
//     `wp_color_management` 声明（含 luminances，见 WaylandColorFeedback）。

#include "HdrOutput.h"
#include "Log.h"
#include "WaylandColorFeedback.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMetaObject>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <rhi/qrhi.h>

#include <cstring>

namespace hdr {

/// libplacebo 的参考白（mpv `video/out/gpu/video.c` 同名常量；wayland_common.c
/// 的 info_done 按它把合成器亮度换算到 libplacebo 口径）。
static constexpr float PL_SDR_WHITE = 203.0f;

const char *modeName(Mode m) {
    switch (m) {
    case Mode::Hdr10: return "HDR10";
    case Mode::ScRgb: return "scRGB";
    case Mode::P3:    return "P3";
    case Mode::Sdr:   break;
    }
    return "SDR";
}

VkFormat vkFormatForMode(Mode m) {
    switch (m) {
    case Mode::Hdr10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Mode::ScRgb: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Mode::P3:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Mode::Sdr:   break;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

VkFormat renderTargetFormatForMode(Mode m) {
    switch (m) {
    case Mode::Hdr10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Mode::ScRgb: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Mode::P3:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Mode::Sdr:   break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;   // 现状：Video.cpp 的硬编码值
}

const char *qtHdrFormatProperty(Mode m) {
    switch (m) {
    case Mode::Hdr10: return "hdr10";
    case Mode::ScRgb: return "scrgb";
    case Mode::P3:    return "p3";
    case Mode::Sdr:   break;
    }
    return nullptr;
}

Policy policyFromEnv() {
    const QByteArray v = qgetenv("VSR_HDR").trimmed().toLower();
    if (v.isEmpty() || v == "auto" || v == "1")
        return Policy::Auto;
    if (v == "off" || v == "0" || v == "sdr")
        return Policy::Off;
    if (v == "hdr10")
        return Policy::ForceHdr10;
    MLOG_ERR("hdr: VSR_HDR=%s 无法识别（支持 off|auto|hdr10）→ 按 auto 处理",
             v.constData());
    return Policy::Auto;
}

Mode requestedModeFromEnv() {   // 兼容旧接口：强制 hdr10 → Hdr10，其余 → Sdr
    return policyFromEnv() == Policy::ForceHdr10 ? Mode::Hdr10 : Mode::Sdr;
}

static QString backendName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Vulkan:      return QStringLiteral("Vulkan");
    case QSGRendererInterface::OpenGL:      return QStringLiteral("OpenGL");
    case QSGRendererInterface::Direct3D11:  return QStringLiteral("D3D11");
    case QSGRendererInterface::Direct3D12:  return QStringLiteral("D3D12");
    case QSGRendererInterface::Metal:       return QStringLiteral("Metal");
    case QSGRendererInterface::Null:        return QStringLiteral("Null");
    default:                                break;
    }
    return QStringLiteral("?");
}

static Mode modeForRhiFormat(QRhiSwapChain::Format f) {
    switch (f) {
    case QRhiSwapChain::HDR10:                      return Mode::Hdr10;
    case QRhiSwapChain::HDRExtendedSrgbLinear:      return Mode::ScRgb;
    case QRhiSwapChain::HDRExtendedDisplayP3Linear: return Mode::P3;
    case QRhiSwapChain::SDR:                        break;
    }
    return Mode::Sdr;
}

// ── 显示能力模块（libvsr-color-feedback.so，运行时 dlopen）──────────────
// 与 WaylandIdleInhibit/SessionIdle 同一模式：模块缺失（无 qt6-wayland /
// 非 Wayland）只降级功能（读不到 → 退 SDR），不影响主程序启动。
namespace {

using init_fn      = int (*)(void *qwindow);
using get_fn       = int (*)(vsr_color_pref *);
using declare_fn   = int (*)(void *qwindow, const vsr_hdr_meta *meta);
using unset_fn     = int (*)(void *qwindow);
using summary_fn   = const char *(*)(void);
using cb_fn        = void (*)(void (*)(void *), void *);
using shutdown_fn  = void (*)(void);

struct ColorModule {
    QLibrary *lib = nullptr;
    init_fn      init = nullptr;
    get_fn       get = nullptr;
    declare_fn   declarePq = nullptr;
    unset_fn     unset = nullptr;
    summary_fn   summary = nullptr;
    cb_fn        setChangeCb = nullptr;
    shutdown_fn  shutdown = nullptr;
    bool         tried = false;
    int          initRc = 99;   ///< 99 = 未尝试
};

QString colorModulePath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList cands = {
        appDir + QStringLiteral("/libvsr-color-feedback.so"),
        QDir(appDir + QStringLiteral("/../lib/vsr-player")).absolutePath()
            + QStringLiteral("/libvsr-color-feedback.so"),
    };
    for (const QString &p : cands)
        if (QFileInfo::exists(p))
            return p;
    return QString();
}

ColorModule &colorModule() {
    static ColorModule m;
    if (!m.tried) {
        m.tried = true;
        const QString path = colorModulePath();
        if (path.isEmpty()) {
            MLOG_INFO("hdr: 显示能力模块未找到（libvsr-color-feedback.so）→ "
                      "显示能力不可读（退 SDR）");
            return m;
        }
        m.lib = new QLibrary(path);
        if (!m.lib->load()) {
            MLOG_INFO("hdr: 显示能力模块加载失败 (%s): %s",
                      path.toUtf8().constData(),
                      m.lib->errorString().toUtf8().constData());
            delete m.lib;
            m.lib = nullptr;
            return m;
        }
        m.init      = reinterpret_cast<init_fn>(m.lib->resolve("vsr_color_fb_init"));
        m.get       = reinterpret_cast<get_fn>(m.lib->resolve("vsr_color_fb_get"));
        m.declarePq = reinterpret_cast<declare_fn>(m.lib->resolve("vsr_color_fb_declare_pq"));
        m.unset     = reinterpret_cast<unset_fn>(m.lib->resolve("vsr_color_fb_unset"));
        m.summary   = reinterpret_cast<summary_fn>(m.lib->resolve("vsr_color_fb_summary"));
        m.setChangeCb = reinterpret_cast<cb_fn>(m.lib->resolve("vsr_color_fb_set_change_callback"));
        m.shutdown  = reinterpret_cast<shutdown_fn>(m.lib->resolve("vsr_color_fb_shutdown"));
    }
    return m;
}

/// 懒初始化（需要窗口已 show → wl_surface 存在）。返回 0 = 可用。
int ensureModule(QQuickWindow *window) {
    ColorModule &m = colorModule();
    if (!m.init || !m.get)
        return -1;
    // -2 = 还没准备好（wl_surface 未建 / Qt 的 wp_color_manager_v1 全局还没绑上）
    // → **每次重试**（模块内是幂等的：订阅过就直接返回 0），不能只试一次。
    if (m.initRc == 99 || m.initRc == -2) {
        const int rc = m.init(window);
        m.initRc = rc;
        if (rc == 0) {
            MLOG_INFO("hdr: 合成器色彩管理就绪：%s",
                      m.summary ? m.summary() : "(no summary)");
        } else if (rc != -2) {
            MLOG_INFO("hdr: 显示能力不可用（rc=%d）→ 退 SDR", rc);
            m.initRc = -1;
        }
    }
    return m.initRc;
}

}  // namespace（模块加载/懒初始化部分）

DisplayInfo displayInfoFor(QQuickWindow *window) {
    DisplayInfo d;
    if (!window) return d;
    if (QScreen *s = window->screen()) d.screen = s->name();
    if (ensureModule(window) != 0)
        return d;
    ColorModule &m = colorModule();
    if (!m.get)
        return d;

    vsr_color_pref w{};
    if (m.get(&w) != 0 || !w.valid)
        return d;

    d.valid = true;
    d.iccOnly = w.icc_only != 0;
    d.generation = w.generation;
    d.primaries = w.named_primaries; d.havePrimaries = w.have_named_primaries != 0;
    d.tf = w.tf; d.haveTf = w.have_tf != 0;
    d.minLuma = (float)w.min_lum;
    d.maxLuma = (float)w.max_lum;
    d.refLuma = (float)w.ref_lum;
    d.haveLuminances = w.have_luminances != 0;
    d.targetMinLuma = (float)w.target_min_lum;
    d.targetMaxLuma = (float)w.target_max_lum;
    d.haveTarget = w.have_target != 0;
    d.gamutArea = (float)w.gamut_area;
    d.hdr = w.hdr_volume != 0;
    if (d.hdr)
        d.reason = QStringLiteral("gamut_area=%1（sRGB=0.112）tf=%2 named_prim=%3")
                       .arg(d.gamutArea, 0, 'f', 4).arg(int(d.tf))
                       .arg(d.havePrimaries ? int(d.primaries) : -1);
    else
        d.reason = QStringLiteral("gamut_area=%1 tf=%2（sRGB/BT.709 级）")
                       .arg(d.gamutArea, 0, 'f', 4).arg(int(d.tf));

    // 亮度换算对齐 mpv（wayland_common.c info_done）：合成器亮度以 ref_lum 为
    // 参考，libplacebo 固定参考白 PL_SDR_WHITE(203) → 线性缩放。min 不缩放。
    // ⚠ 该值**不能**填进 ra_fbo.color_space 当峰值（会把 HDR 内容预 tone map
    // 到 203nit → 画面暗 2.4 倍，实测），当前仅用于日志。
    const float a = d.minLuma, c = d.minLuma;
    if (d.haveLuminances && d.refLuma > a) {
        const float b = (PL_SDR_WHITE - c) / (d.refLuma - a);
        d.peakNits = (d.maxLuma - a) * b + c;
        d.minNits = d.minLuma;
    } else {
        d.peakNits = d.maxLuma;
        d.minNits = d.minLuma;
    }
    return d;
}

// ── 策略状态机（GUI 线程；显示能力 + 内容两路输入 → 目标模式）──────────
namespace {

struct PolicyState {
    QQuickWindow *window = nullptr;
    ApplyHooks   hooks;
    Policy       policy = Policy::Auto;
    bool         contentHdr = false;
    ContentMeta  contentMeta;      ///< 内容静态元数据（HDR10 描述用）
    DisplayInfo  display;          ///< 最近一次合成器快照
    Mode         applied = Mode::Sdr;
    bool         haveApplied = false;
    QString      shortReason;      ///< 简短原因（OSD；英文，不翻译）
};

PolicyState &policyState() { static PolicyState s; return s; }

QRhiSwapChain::Format rhiFormatForMode(Mode m) {
    switch (m) {
    case Mode::Hdr10: return QRhiSwapChain::HDR10;
    case Mode::ScRgb: return QRhiSwapChain::HDRExtendedSrgbLinear;
    case Mode::P3:    return QRhiSwapChain::HDRExtendedDisplayP3Linear;
    case Mode::Sdr:   break;
    }
    return QRhiSwapChain::SDR;
}

void reevaluate(const char *why);

uint64_t g_lastNotifiedGen = 0;

void logDisplayInfo(QQuickWindow *window, const char *when) {
    const DisplayInfo d = displayInfoFor(window);
    if (!d.valid) {
        MLOG_INFO("hdr display[%s]: screen=%s 合成器显示能力不可读（模块缺失/无 "
                  "wp_color_management_v1/还没拿到 preferred 描述）→ 退 SDR",
                  when, d.screen.toUtf8().constData());
        return;
    }
    MLOG_INFO("hdr display[%s]: screen=%s gamut_area=%.4f primaries=%d tf=%d "
              "luminances{min=%.4f max=%.1f ref=%.1f}%s → %s（%s）"
              " mpv口径 peak=%.1f nit min=%.2f nit",
              when, d.screen.toUtf8().constData(), d.gamutArea,
              d.havePrimaries ? int(d.primaries) : -1,
              d.haveTf ? int(d.tf) : -1,
              d.minLuma, d.maxLuma, d.refLuma,
              d.iccOnly ? " [ICC-only]" : "",
              d.hdr ? "HDR-capable" : "SDR",
              d.reason.toUtf8().constData(),
              d.peakNits, d.minNits);
}

/// 新快照（代数变化）→ 喂给策略状态机并重新决策。GUI 线程调用。
void notifyDisplayInfo(QQuickWindow *window) {
    const DisplayInfo d = displayInfoFor(window);
    if (!d.valid || d.generation == g_lastNotifiedGen)
        return;
    g_lastNotifiedGen = d.generation;
    policyState().display = d;
    reevaluate("display");
}

/// 模式**没有变化**时也要同步的两件事（幂等）：
///  · `updateMpvTarget`：mpv 的 target-trc/prim 必须与当前 swapchain/声明一致。
///    `VSR_HDR=hdr10` 是"启动即 Hdr10"（show 前设好），`reevaluate` 的
///    `want == applied` 早退会漏掉这个钩子 → gl_video 不做 SDR→PQ 转换，
///    把 SDR 码值直接写进 PQ 面 → 颜色错（2026-10-17 用户实测：
///    `VSR_HDR=hdr10 input/goose_480p.webm` 颜色不正确）。
///  · 声明：同上，强制模式启动时也要把 HDR10 描述（含内容静态元数据）补上。
/// 显示能力/内容元数据变化都会走到这里，所以重复调用必须安全。
void syncTargets(Mode mode) {
    PolicyState &st = policyState();
    if (st.hooks.updateMpvTarget)
        st.hooks.updateMpvTarget(mode, st.display);
    if (mode == Mode::Hdr10) {
        // Qt 自己的 UI 编码用 QSurfaceFormat::colorSpace；这里只在 platform window
        // **已存在**（首帧后）时设置 → Qt 不会再建 `wp_color_management_surface_v1`
        // （自动切换路径一直如此），color surface 归我们模块独占。
        if (st.window) {
            QSurfaceFormat fmt = st.window->format();
            fmt.setColorSpace(QColorSpace(QColorSpace::Primaries::Bt2020,
                                          QColorSpace::TransferFunction::St2084));
            st.window->setFormat(fmt);
        }
        if (qgetenv("VSR_HDR_DECLARE") != "0") {
            const int rc = declareHdr10Surface(st.window);
            if (rc != 0)
                MLOG_WARN("hdr: HDR10 描述下发失败（rc=%d）", rc);
        }
    }
}

}  // namespace

int unsetSurfaceDescription(QQuickWindow *window) {
    ColorModule &m = colorModule();
    if (!m.unset || ensureModule(window) != 0)
        return -1;
    return m.unset(window);
}

int declareHdr10Surface(QQuickWindow *window) {
    ColorModule &m = colorModule();
    if (!m.declarePq || ensureModule(window) != 0)
        return -1;
    // 内容静态元数据（没有则传 NULL → 合成器按主色体积上限 10000nit 处理）
    const ContentMeta &cm = policyState().contentMeta;
    struct vsr_hdr_meta meta {};
    if (cm.valid) {
        meta.valid = 1;
        meta.min_mastering = cm.minMastering;
        meta.max_mastering = cm.maxMastering;
        meta.max_cll = cm.maxCll;
        meta.max_fall = cm.maxFall;
    }
    return m.declarePq(window, cm.valid ? &meta : nullptr);
}


bool applyMode(QQuickWindow *window, Mode mode, QString *err) {
    auto fail = [err](const char *msg) {
        if (err) *err = QString::fromUtf8(msg);
        return false;
    };
    if (!window) return fail("no window");

    // 目标 SDR 且从未声明过色彩空间 → 什么都不做（也不需要 swapchain）。
    // 保住"SDR 现状逐字节不变"：不设属性、不碰 QSurfaceFormat、不发声明；
    // 只有 HDR10→SDR 的回落才需要撤销声明与重建 swapchain。
    if (mode == Mode::Sdr && !window->property("_qt_sg_hdr_format").isValid())
        return true;

    QRhiSwapChain *sc = window->swapChain();
    if (!sc) return fail("swapchain 未就绪（场景图还没建）");

    const QRhiSwapChain::Format want = rhiFormatForMode(mode);
    const Mode cur = modeForRhiFormat(sc->format());

    // ① 窗口属性：Qt 只在自己重建 swapchain 时读它（本次切换自己驱动）
    if (mode == Mode::Sdr) window->setProperty("_qt_sg_hdr_format", QVariant());
    else                   window->setProperty("_qt_sg_hdr_format",
                                              QString::fromLatin1(qtHdrFormatProperty(mode)));

    // ② 色彩空间声明（公开 API）：改 QSurfaceFormat::colorSpace 后必须重新下发
    //（Qt 只在 surface 创建时发一次）。
    QSurfaceFormat fmt = window->format();
    fmt.setColorSpace(mode == Mode::Hdr10
        ? QColorSpace(QColorSpace::Primaries::Bt2020,
                      QColorSpace::TransferFunction::St2084)
        : QColorSpace(QColorSpace::NamedColorSpace::SRgb));
    window->setFormat(fmt);
    // 声明由我们自己下发（不走 Qt 的 initializeColorSpace）：
    //  · HDR10 → 按 mpv 语义声明 PQ/BT.2020 + luminances(0,10000,203)；
    //  · SDR   → 撤下自定义描述（合成器回到默认）。
    // `window->setFormat(colorSpace)` 仍然要设：Qt 自己的 UI 编码用它。
    // HDR10 描述是否由我们下发（`VSR_HDR_DECLARE=0` 关闭，仅作对照）。
    // **默认 1 = 我们自己下发**（bt2020 + st2084_pq + luminances 0/10000/203，
    // mpv wayland_common.c 的 PQ 分支语义）。用户目检确认（2026-10-17）：
    // 声明路径"画面正常"；不声明（= mpv `--target-colorspace-hint` 的未声明
    // 路径）在本机**画面发灰**（PQ 缓冲被按 gamma2.2 解释 → 更亮但饱和/对比塌）。
    // ⚠ 教训：**平均亮度不能当正确性判据** —— 当时测得"不声明 19.6 vs 声明 13.1，
    //    mpv 20.7"，据此选错；发灰正是"更亮"的观感来源。必须目检（或比饱和度/对比度）。
    static const bool declareEnabled = qgetenv("VSR_HDR_DECLARE") != "0";    if (mode == Mode::Hdr10 && !declareEnabled) {
        MLOG_INFO("hdr applyMode: 不下发自定义色彩描述（VSR_HDR_DECLARE=0，"
                  "对齐 mpv hint 路径；本机实测画面发灰，仅作对照）");
        unsetSurfaceDescription(window);
        if (err) err->clear();
    } else if (mode == Mode::Hdr10) {
        const int drc = declareHdr10Surface(window);
        if (drc != 0)
            MLOG_WARN("hdr applyMode: PQ 声明失败（rc=%d）——合成器可能按默认/首选"
                      "解释本窗口（可能偏暗）", drc);
    } else {
        const int urc = unsetSurfaceDescription(window);
        if (urc != 0)
            MLOG_WARN("hdr applyMode: 撤下色彩描述失败（rc=%d）", urc);
    }

    // ③ swapchain 格式（需要重建）
    if (cur != mode) {
        if (!sc->isFormatSupported(want))
            return fail("合成器/display 不支持该 swapchain 格式");
        sc->setFormat(want);
        QRhiRenderPassDescriptor *rp = sc->newCompatibleRenderPassDescriptor();
        if (!rp) return fail("newCompatibleRenderPassDescriptor 失败");
        sc->setRenderPassDescriptor(rp);
        if (!sc->createOrResize()) return fail("swapchain createOrResize 失败");
        MLOG_INFO("hdr applyMode: swapchain %s → %s（已重建，含 render pass）",
                  modeName(cur), modeName(mode));
    }
    return true;
}

namespace {

void reevaluate(const char *why) {
    PolicyState &st = policyState();
    if (!st.window) return;

    Mode want = Mode::Sdr;
    QString reason;
    switch (st.policy) {
    case Policy::ForceHdr10:
        want = Mode::Hdr10;
        reason = QStringLiteral("策略 hdr10（强制）");
        st.shortReason = QStringLiteral("forced");
        break;
    case Policy::Auto:
        if (!st.display.valid) {
            reason = QStringLiteral("显示能力未知（preferred 未到）→ 退 SDR");
            st.shortReason = QStringLiteral("display unknown");
        } else if (!st.display.hdr) {
            reason = QStringLiteral("合成器 preferred 非宽色域（SDR 屏或 WCG-only）"
                                    " gamut_area=%1").arg(st.display.gamutArea, 0, 'f', 4);
            st.shortReason = QStringLiteral("display SDR/WCG-only");
        } else if (!st.contentHdr) {
            reason = QStringLiteral("内容非 HDR（video-params/gamma 非 pq/hlg）");
            st.shortReason = QStringLiteral("content SDR");
        } else {
            want = Mode::Hdr10;
            reason = QStringLiteral("内容 HDR × 显示 HDR（gamut_area=%1，peak=%2 nit）")
                         .arg(st.display.gamutArea, 0, 'f', 4)
                         .arg(st.display.peakNits, 0, 'f', 1);
            st.shortReason = QStringLiteral("content HDR x display HDR");
        }
        break;
    case Policy::Off:
    default:
        reason = QStringLiteral("策略 off");
        st.shortReason = QStringLiteral("policy off");
        break;
    }

    if (st.haveApplied && want == st.applied) {
        // 模式未变，但钩子的输入（显示能力/内容元数据）可能变了 → 仍要同步
        // （强制模式 `VSR_HDR=hdr10` 的 mpv target/声明就靠这里补上）。
        syncTargets(want);
        if (st.hooks.statusChanged)
            st.hooks.statusChanged();
        return;
    }

    QString err;
    if (!applyMode(st.window, want, &err)) {
        MLOG_ERR("hdr policy[%s]: 目标 %s 应用失败（%s）→ 保持 %s",
                 why, modeName(want), err.toUtf8().constData(), modeName(st.applied));
        return;
    }
    st.applied = want;
    st.haveApplied = true;
    MLOG_INFO("hdr policy[%s]: → %s（%s）", why, modeName(want),
              reason.toUtf8().constData());
    if (st.hooks.rebuildRenderPath)
        st.hooks.rebuildRenderPath(want);
    if (st.hooks.updateMpvTarget)
        st.hooks.updateMpvTarget(want, st.display);
    if (st.hooks.statusChanged)
        st.hooks.statusChanged();
}

}  // namespace

void installPolicy(QQuickWindow *window, Policy policy, Mode initialMode, ApplyHooks hooks) {
    PolicyState &st = policyState();
    st.window = window;
    st.policy = policy;
    st.applied = initialMode;
    st.haveApplied = true;   // 启动模式已经生效（show 前设的），无需重复应用
    st.hooks = std::move(hooks);
    MLOG_INFO("hdr: 策略=%s（auto = 内容 HDR × 显示能力 HDR；off/hdr10 见文档）",
              policy == Policy::Auto ? "auto" : policy == Policy::Off ? "off" : "hdr10");
    reevaluate("install");
}

void setContentHdr(bool hdr) {
    PolicyState &st = policyState();
    if (st.contentHdr == hdr)
        return;
    st.contentHdr = hdr;
    reevaluate("content");
}

void setContentMeta(const ContentMeta &meta) {
    PolicyState &st = policyState();
    const ContentMeta &cur = st.contentMeta;
    if (cur.valid == meta.valid && cur.minMastering == meta.minMastering &&
        cur.maxMastering == meta.maxMastering && cur.maxCll == meta.maxCll &&
        cur.maxFall == meta.maxFall)
        return;
    st.contentMeta = meta;

    if (!meta.valid) {
        MLOG_INFO("hdr content-meta: 无静态元数据 → 合成器按主色体积上限"
                  "（PQ 名义 10000nit）做 tone mapping");
    } else {
        MLOG_INFO("hdr content-meta: mastering %.4f-%.0f nit, max_cll=%.0f, "
                  "max_fall=%.0f → 作为 HDR10 描述的内容峰值下发",
                  meta.minMastering, meta.maxMastering, meta.maxCll, meta.maxFall);
    }

    // 元数据变化就是状态文本变化（content Xnit）→ 立即刷新 OSD。
    // ⚠ 必须**先**刷新再走下面的早退：切 HDR10 与元数据到达可能在同一瞬间发生
    //（实测从播放列表续播时 statusChanged 先跑，文本停在 `content ?`）。
    if (st.hooks.statusChanged)
        st.hooks.statusChanged();

    // 只在"当前确实在 HDR10 且声明启用"时重新下发；off 策略完全不碰声明。
    if (st.policy == Policy::Off || st.applied != Mode::Hdr10)
        return;
    if (qgetenv("VSR_HDR_DECLARE") == "0")
        return;
    const int rc = declareHdr10Surface(st.window);
    if (rc != 0)
        MLOG_WARN("hdr content-meta: 重新声明描述失败（rc=%d）", rc);
}

Mode targetMode() { return policyState().applied; }

QString statusText() {
    PolicyState &st = policyState();
    const ContentMeta &cm = st.contentMeta;
    // 内容峰值：优先 MaxCLL（= KWin 用的口径），退回 mastering max
    const double contentPeak = cm.maxCll > 0 ? cm.maxCll : cm.maxMastering;
    const bool haveContent = cm.valid && contentPeak > 0;
    QString contentStr = haveContent
        ? QStringLiteral("%1nit").arg(contentPeak, 0, 'f', 0)
        : QStringLiteral("HDR");   // HDR 片源但暂无静态元数据（合成器按 10000nit 口径）

    if (st.applied == Mode::Hdr10) {
        QStringList parts;
        parts << QStringLiteral("HDR10 · BT.2020/PQ");
        if (st.display.valid && st.display.maxLuma > 0)
            parts << QStringLiteral("display %1nit").arg(st.display.maxLuma, 0, 'f', 0);
        parts << (st.contentHdr ? QStringLiteral("content %1").arg(contentStr)
                                : QStringLiteral("content SDR"));
        if (st.policy == Policy::ForceHdr10)
            parts << QStringLiteral("(forced)");
        return parts.join(QStringLiteral(" · "));
    }
    // SDR 输出
    if (!st.contentHdr)
        return QStringLiteral("SDR · content SDR%1").arg(
            st.policy == Policy::ForceHdr10 ? QStringLiteral(" (forced)") : QString());
    return QStringLiteral("SDR · content HDR not displayed (%1)")
        .arg(st.shortReason.isEmpty() ? QStringLiteral("?") : st.shortReason);
}

// ── 只读探测 ──────────────────────────────────────────────────────────
Capabilities probeSwapchain(QQuickWindow *window) {
    Capabilities caps;
    if (!window) return caps;

    if (auto *ri = window->rendererInterface())
        caps.backend = backendName(ri->graphicsApi());
    if (auto *scr = window->screen())
        caps.screen = scr->name();

    QRhiSwapChain *sc = window->swapChain();
    if (!sc) return caps;   // 场景图/swapchain 尚未建立

    caps.probed = true;
    caps.hdr10 = sc->isFormatSupported(QRhiSwapChain::HDR10);
    caps.scrgb = sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
    caps.p3    = sc->isFormatSupported(QRhiSwapChain::HDRExtendedDisplayP3Linear);
    caps.current = modeForRhiFormat(sc->format());
    // fallback 链：HDR10 → scRGB → SDR（P3 只记录，不进链）
    caps.preferred = caps.hdr10 ? Mode::Hdr10
                   : caps.scrgb ? Mode::ScRgb
                                : Mode::Sdr;
    return caps;
}

namespace {

void logCapabilities(const Capabilities &c, const char *when) {
    if (!c.probed) {
        MLOG_INFO("hdr probe[%s]: swapchain 未就绪（backend=%s screen=%s）——稍后重试",
                  when, c.backend.toUtf8().constData(), c.screen.toUtf8().constData());
        return;
    }
    MLOG_INFO("hdr probe[%s]: backend=%s screen=%s swapchain=%s "
              "supported{HDR10=%d scRGB=%d P3=%d} → preferred=%s",
              when, c.backend.toUtf8().constData(), c.screen.toUtf8().constData(),
              modeName(c.current), c.hdr10, c.scrgb, c.p3, modeName(c.preferred));
}

}  // namespace

void installCapabilityProbe(QQuickWindow *window) {
    if (!window) return;

    // 合成器描述变化（KWin 切 HDR / 换屏 / 插拔）→ 模块已用 QueuedConnection
    // 转投 GUI 线程，这里重新读并打日志 + 触发策略重算。
    ColorModule &cm = colorModule();
    if (cm.setChangeCb)
        cm.setChangeCb([](void *user) {
            auto *w = static_cast<QQuickWindow *>(user);
            QMetaObject::invokeMethod(w, [w] {
                logDisplayInfo(w, "compositor changed");
                notifyDisplayInfo(w);
            }, Qt::QueuedConnection);
        }, window);

    // 首帧探测：swapchain 在这里才绑定到窗口（m_window 非空），
    // 之前调用 isFormatSupported 会打 Qt 警告且查不到 surface。
    auto *firstDone = new bool(false);
    QObject::connect(window, &QQuickWindow::afterRendering, window,
                     [window, firstDone]() {
        if (*firstDone) return;
        *firstDone = true;
        logCapabilities(probeSwapchain(window), "initial");
        logDisplayInfo(window, "initial");
        notifyDisplayInfo(window);
        // 首帧后 wl_surface 一定存在 → 强制模式（启动即 Hdr10，没走 applyMode）
        // 在这里补一次 mpv target + HDR10 描述下发（幂等）。
        reevaluate("first-frame");
        // hdrInfo() 在 Linux 是 Qt 硬编码默认值——记录下来证明"不能用它当选屏依据"。
        if (QRhiSwapChain *sc = window->swapChain()) {
            const QRhiSwapChainHdrInfo hi = sc->hdrInfo();
            if (hi.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits)
                MLOG_INFO("hdr probe: Qt hdrInfo (Linux 恒为硬编码默认值，不可作为"
                          "选屏依据) limits=nits min=%.2f max=%.2f sdrWhite=%.2f "
                          "behavior=%s",
                          hi.limits.luminanceInNits.minLuminance,
                          hi.limits.luminanceInNits.maxLuminance,
                          hi.sdrWhiteLevel,
                          hi.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred
                              ? "scene-referred" : "display-referred");
            else
                MLOG_INFO("hdr probe: Qt hdrInfo limits=color-component max=%.2f "
                          "potential=%.2f sdrWhite=%.2f",
                          hi.limits.colorComponentValue.maxColorComponentValue,
                          hi.limits.colorComponentValue.maxPotentialColorComponentValue,
                          hi.sdrWhiteLevel);
        }
    });

    // 换屏重探：isFormatSupported 是 per-surface，显示能力也随屏变化。
    QObject::connect(window, &QWindow::screenChanged, window,
                     [window](QScreen *) {
        logCapabilities(probeSwapchain(window), "screen changed");
        logDisplayInfo(window, "screen changed");
        notifyDisplayInfo(window);
    });
}

void installSwapchainVerification(QQuickWindow *window, Mode expected) {
    if (!window) return;
    auto *done = new bool(false);
    QObject::connect(window, &QQuickWindow::afterRendering, window,
                     [window, expected, done]() {
        if (*done) return;
        const Capabilities c = probeSwapchain(window);
        if (!c.probed) return;   // swapchain 还没绑定：等下一帧（不刷屏）
        *done = true;
        if (c.current == expected) {
            MLOG_INFO("hdr verify: swapchain=%s == 期望 %s ✔（screen=%s）",
                      modeName(c.current), modeName(expected),
                      c.screen.toUtf8().constData());
        } else {
            MLOG_ERR("hdr verify: swapchain=%s ≠ 期望 %s（Qt 回退了："
                     "isFormatSupported{HDR10=%d scRGB=%d P3=%d} backend=%s screen=%s）"
                     "—— 强制模式不回退，继续按期望格式渲染，画面可能错乱",
                     modeName(c.current), modeName(expected),
                     c.hdr10, c.scrgb, c.p3, c.backend.toUtf8().constData(),
                     c.screen.toUtf8().constData());
        }
    });
}

} // namespace hdr
