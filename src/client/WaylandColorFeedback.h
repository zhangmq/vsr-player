#pragma once

/// Wayland 合成器**表面色彩偏好**读取模块（libvsr-color-feedback.so）的 C ABI。
///
/// 为什么是这个信号（2026-10-17 实测，见 docs/hdr-pipeline-investigation.md §12）：
///   - `QRhiSwapChain::isFormatSupported()` 在 Wayland 上查的是**合成器整体**支持的
///     format/colorspace 组合，SDR 屏（DP-2）也返回 HDR10=1 → 不能用来判断"这块屏
///     是不是 HDR"。
///   - `wp_color_management_v1` 的 **output** image description 也不区分：
///     本机 DP-3 在 HDR 打开（bt2020/400nit/ref400）与 WCG-only（HDR 关、WCG 开）下
///     描述**逐字段相同**（都是 bt2020/gamma2.2/0–400/ref400）。
///   - 真正区分的是合成器给**这块表面**的 preferred description（mpv 的
///     `--target-colorspace-hint` 走的就是它）：HDR 开 → `bt2020`（400nit）；
///     WCG-only → **`bt709`**（200nit）。所以判据必须来自 surface feedback。
///
/// 实现：**不自己写协议客户端**——Qt 6.12 的 QtWaylandClient 已完整实现
/// `wp_color_management_v1`（`QtWaylandClient::ColorManager` /
/// `ColorManagementFeedback` / `ImageDescriptionInfo`，见私有头
/// `<QtWaylandClient/private/qwaylandcolormanagement_p.h>`），我们复用它
/// （与 `WaylandIdleInhibit.cpp` 同一"独立模块 + dlopen"模式：QtWaylandClient/
/// 私有头不硬链进主程序，缺 qt6-wayland 的系统上启动不受影响）。
///
/// 失败一律返回负值 → 调用方退 SDR（探测不到就不启用 HDR，不假设）。
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/// 合成器给该表面的 preferred image description（快照）。
struct vsr_color_pref {
    int      valid;            ///< 1 = 收到过 preferred 描述
    uint64_t generation;       ///< 每次 preferred 变化 +1

    uint32_t tf;               ///< wp_color_manager_v1 transfer_function 枚举
    int      have_tf;
    uint32_t named_primaries;  ///< wp_color_manager_v1 primaries 枚举（诊断）
    int      have_named_primaries;

    double   min_lum;          ///< 亮度（nit，合成器口径；min 是协议值/10000）
    double   max_lum;
    double   ref_lum;
    int      have_luminances;

    double   target_min_lum;   ///< 目标（面板）亮度范围
    double   target_max_lum;
    uint32_t max_cll;          ///< 目标内容的 MaxCLL/MaxFALL（诊断，非必需）
    uint32_t max_fall;
    int      have_target;

    double   prim[8];          ///< 容器基色 rx ry gx gy bx by wx wy（xy，0..1）
    int      have_primaries;

    double   gamut_area;       ///< 基色三角面积（xy）：sRGB≈0.112 / P3≈0.152 / BT.2020≈0.211
    int      hdr_volume;       ///< 派生判据：tf 是 PQ/HLG，或 gamut_area 明显大于 sRGB
    int      icc_only;         ///< 1 = 描述只有 ICC 文件（本模块不解析 → 参数不可用）
};

/// 初始化：取 qwindow（QWindow*）的 wl_surface，通过 Qt 的 ColorManager 建立
/// surface feedback 并监听 preferred 变化。返回 0 成功；
/// -1 参数/内部错误；-2 非 Wayland 平台（无 QtWaylandClient 窗口）；
/// -3 合成器不支持 wp_color_management_v1。
int vsr_color_fb_init(void *qwindow);

/// 读最近一次 preferred 快照。返回 0 = 已写入（`out->valid` 说明是否收到过）。
int vsr_color_fb_get(struct vsr_color_pref *out);

/// 撤下自定义色彩描述 → 合成器回到默认/首选解释（HDR10 → SDR 回落用）。
/// 返回 0 = 已处理；负值 = 不可用。
int vsr_color_fb_unset(void *qwindow);

/// 内容侧 HDR10 **静态元数据**（来自 mpv `video-params/{min-luma,max-luma,max-cll,max-fall}`，
/// 单位 nit）。它决定合成器按什么"内容峰值"做 tone mapping：
///   KWin `src/core/colorpipeline.cpp` 的输入峰值 = image description 的
///   `maxHdrLuminance`，其取值链 = **max_cll → mastering max → 主色体积上限**
///   （PQ 按协议恒为 min+10000）。所以**不发这两个字段**时，合成器只能按
///   10000nit 处理 → 对真实片源（如 mastering 1000nit / MaxCLL 758nit）
///   压缩过狠、对比度变差（实测症状：我们的 HDR 画面比 mpv 平）。
/// 协议语义（wayland-protocols color-management-v1.xml）：
///   - `set_luminances` 的 max_lum 在 PQ 下**被忽略**（恒为 min_lum+10000），
///     所以它表达的是"主色体积"，**不是**内容峰值；
///   - 内容峰值只能靠 `set_mastering_luminance`（ST 2086）与
///     `set_max_cll`/`set_max_fall`（CTA-861-H）表达（mpv `set_color_management()`
///     同款：HDR transfer 且有有效元数据时才发）。
struct vsr_hdr_meta {
    int    valid;           ///< 1 = 元数据有效（min_mastering < max_mastering）
    double min_mastering;   ///< ST 2086 mastering min L（nit）
    double max_mastering;   ///< ST 2086 mastering max L（nit）
    double max_cll;         ///< CTA-861 MaxCLL（nit；0 = 未定义 → 合成器回退 mastering max）
    double max_fall;        ///< CTA-861 MaxFALL（nit；0 = 未定义）
};

/// 按 **mpv 的语义**声明本窗口为 HDR10（PQ/BT.2020）内容：
/// `set_primaries_named(bt2020)` + `set_tf_named(st2084_pq)` +
/// **`set_luminances(0, 10000, 203)`**（mpv wayland_common.c 的 PQ 分支照抄；
/// Qt 的 `QSurfaceFormat::colorSpace` 路径只发前两条，**缺 luminances** →
/// 实测合成器映射结果不同：同帧亮度 14.0 vs mpv 原生 23.1）。
/// `meta`（可 NULL = 无元数据）非空时再补 `set_mastering_luminance` +
/// `set_max_cll`/`set_max_fall`（见 `struct vsr_hdr_meta` 的说明）；
/// 合法性按 mpv `cm_metadata_sanitize()` 同款过滤（min>=max 整体丢弃；
/// v1 合成器额外要求 cll/fall 落在 mastering 区间内；fall<=cll 否则两者清零）。
/// 需要在窗口已有 wl_surface 之后调用；会创建自己的
/// `wp_color_management_surface_v1`。
/// ⚠ **一个 wl_surface 只能有一个 color management surface**（协议
/// `wp_color_manager_v1` error surface_exists）：如果 Qt 已经建过（Qt 在
/// platform window 创建时按 `QSurfaceFormat::colorSpace` 建），我们再
/// `get_surface()` 会触发协议错误 → 连接断开、app 退出（2026-10-17 实测
/// `VSR_HDR=hdr10`）。所以调用时序要求：**窗口创建时不要设 colorSpace**，
/// 让本模块独占；colorSpace 在首帧之后（platform window 已存在）再设。
/// 返回 0 成功；负值 = 不可用（无 ColorManager / 无 wl_surface / 合成器不支持 PQ）。
int vsr_color_fb_declare_pq(void *qwindow, const struct vsr_hdr_meta *meta);

/// 诊断摘要（进程生命周期内有效）。
const char *vsr_color_fb_summary(void);

/// 关闭（销毁 feedback 对象；幂等）。
void vsr_color_fb_shutdown(void);

#ifdef __cplusplus
}
#endif
