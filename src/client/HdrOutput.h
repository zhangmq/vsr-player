#pragma once

// HDR 上屏：能力探测与（后续）格式/色彩空间选择。
//
// 设计约束（docs/hdr-pipeline-investigation.md §10–11，全部有实测证据）：
//  1. HDR swapchain 只在 Qt 的 **Vulkan RHI 后端**生效（GL 后端下
//     isFormatSupported 恒 false、属性被静默忽略）——本项目已经
//     `QSG_RHI_BACKEND=vulkan`（main.cpp），探测结果里会记录后端以免误判。
//  2. 一切能力**运行时探测**，不假设任何具体显示器参数；探测不到 → 保持现状
//     （SDR 8-bit），绝不"假设支持 HDR"。
//  3. `isFormatSupported()` 是 **per-surface**（内部现查
//     vkGetPhysicalDeviceSurfaceFormatsKHR），窗口换屏必须重新评估。
//  4. 真实显示能力（峰值/最小亮度/色域）**不能问 Qt**：Linux 上
//     `QRhiSwapChain::hdrInfo()` 是 Qt 硬编码默认值（min 0 / max 1000 nit /
//     sdrWhiteLevel 200，只有 Windows 有真实实现）→ 需自建
//     `wp_color_management` 客户端读合成器（后续步骤）。
//
// 与 Qt 的私有头依赖：`<rhi/qrhi.h>` 装在 qtbase 私有头目录
// （meson.build 的 _qt6_priv_inc 已处理；QRhiSwapChain::format() 是内联、
// isFormatSupported 是纯虚，只有头文件依赖，没有私有符号链接）。

#include <QString>
#include <functional>
#include <vulkan/vulkan.h>

class QQuickWindow;

namespace hdr {

/// 上屏色彩空间档位。Sdr = 现状（8-bit，Qt 默认 SDR swapchain）。
enum class Mode {
    Sdr = 0,
    Hdr10,      ///< A2B10G10R10_UNORM_PACK32 + HDR10_ST2084（首选）
    ScRgb,      ///< R16G16B16A16_SFLOAT + EXTENDED_SRGB_LINEAR（fallback）
    P3,         ///< R16G16B16A16_SFLOAT + DISPLAY_P3_LINEAR（探测记录用）
};

const char *modeName(Mode m);

/// Qt 对某个 swapchain 格式实际使用的 VkFormat（对照 Qt 源码 qrhivulkan.cpp
/// 的 hdrFormatMatchesVkSurfaceFormat / defaultColorFormat；SDR 取
/// B8G8R8A8_UNORM——即本项目 CompositePipeline 现在的硬编码值，改动时须复核）。
VkFormat vkFormatForMode(Mode m);

/// 单个窗口（= 单个 wl_surface / 单块显示器）上探测到的 HDR 能力。
struct Capabilities {
    bool probed = false;   ///< false = 还没拿到 swapchain（≠"不支持"）
    bool hdr10 = false;
    bool scrgb = false;
    bool p3 = false;
    Mode current = Mode::Sdr;     ///< 当前 swapchain 实际格式
    Mode preferred = Mode::Sdr;   ///< HDR10 → scRGB → SDR
    QString backend;              ///< QRhi 后端名（GL 后端下三项恒 false）
    QString screen;               ///< QScreen::name()（诊断/多屏）
};

/// 合成器给出的**真实**显示能力（来自 wp_color_management_v1 的 output
/// image description；不走 Qt）。数值单位 nit（min_lum_1e4 已换算）。
/// 亮度换算对齐 mpv（`wayland_common.c` info_done 的 libplacebo 参考白缩放，
/// PL_COLOR_SDR_WHITE=203）：`peakNits`/`minNits` 可直接喂给 mpv 的 target。
struct DisplayInfo {
    bool     valid = false;     ///< false = 没读到（模块缺失/合成器无协议）→ 必须退 SDR
    bool     iccOnly = false;   ///< 描述只有 ICC 文件（本模块不解析）→ 参数不可用
    bool     hdr = false;       ///< 派生：该输出按合成器描述可做 HDR 输出
    QString  reason;            ///< hdr 判定的依据（日志/证据）
    QString  screen;
    uint64_t generation = 0;

    uint32_t primaries = 0; bool havePrimaries = false;   ///< 命名基色（诊断）
    float    gamutArea = 0;                               ///< 基色三角面积（判据）
    uint32_t tf = 0;        bool haveTf = false;
    bool     haveLuminances = false;
    float    minLuma = 0, maxLuma = 0, refLuma = 0;       ///< 原始（合成器口径，nit）
    bool     haveTarget = false;
    float    targetMinLuma = 0, targetMaxLuma = 0, maxCll = 0, maxFall = 0;
    float    peakNits = 0, minNits = 0;                   ///< 换算到 libplacebo 参考白后
};

/// 读合成器给**当前窗口表面**的 preferred 描述（真实显示能力）。
/// 首次调用会懒初始化模块（需要窗口已 show → wl_surface 存在）；模块不可用 /
/// 还没收到描述 → `valid=false`（调用方退 SDR，不假设）。
/// 每次调用都取最新快照。
DisplayInfo displayInfoFor(QQuickWindow *window);

/// 从环境变量 `VSR_HDR` 读**强制**上屏模式（本切片：强制模式，不做自动决策）。
/// 取值：未设置/空/`off` → `Mode::Sdr`（= 现有行为，一个字节都不变）；
/// `hdr10` → `Mode::Hdr10`。其余值记日志并退 Sdr（不猜）。
Mode requestedModeFromEnv();

/// Qt 动态属性 `_qt_sg_hdr_format` 的取值（"hdr10"/"scrgb"/"p3"）；
/// `Mode::Sdr` → nullptr（不设属性 = Qt 用 SDR swapchain）。
const char *qtHdrFormatProperty(Mode m);

/// **rtImage / mpv 渲染目标**的 VkFormat：SDR = R8G8B8A8_UNORM（现状），
/// HDR10 = A2B10G10R10_UNORM_PACK32。注意它与 vkFormatForMode() 不同——
/// 后者是 **Qt swapchain / composite attachment** 格式（SDR = B8G8R8A8）。
VkFormat renderTargetFormatForMode(Mode m);

/// 撤下自定义色彩描述（HDR10 → SDR 回落；回到合成器默认解释）。
int unsetSurfaceDescription(QQuickWindow *window);

/// 输出策略（`VSR_HDR=off|auto|hdr10`；auto = 内容 HDR × 显示能力 HDR 才切）。
enum class Policy { Off, Auto, ForceHdr10 };
Policy policyFromEnv();

/// 内容侧 HDR10 **静态元数据**（nit）。它决定合成器做 tone mapping 时假设的
/// "内容峰值"：KWin 的取值链 = **max_cll → mastering max → 主色体积上限**
/// （PQ 按协议恒为 10000nit，见 wayland-protocols color-management-v1.xml 的
/// set_luminances 说明）。缺这两个字段时合成器只能按 10000nit 压 → 真实片源
/// （普遍 mastering≈1000nit）会被压得过狠、对比度变差。
/// 来源：mpv `video-params/{min-luma,max-luma,max-cll,max-fall}`。
struct ContentMeta {
    bool   valid = false;    ///< false = 无元数据（合成器回退 10000nit 口径）
    double minMastering = 0; ///< ST 2086 mastering min L
    double maxMastering = 0; ///< ST 2086 mastering max L
    double maxCll = 0;       ///< CTA-861 MaxCLL（0 = 未定义）
    double maxFall = 0;      ///< CTA-861 MaxFALL（0 = 未定义）
};

/// 更新内容静态元数据（每次换片/元数据到达时调用）。幂等；仅在**当前已应用
/// HDR10 且声明启用**（`VSR_HDR_DECLARE != 0`）时重新下发色彩描述。
void setContentMeta(const ContentMeta &meta);

/// 按 mpv 语义声明窗口为 HDR10（PQ/BT.2020 + luminances 0/10000/203 +
/// 已知的内容静态元数据）。实测这是"HDR 画面亮度与 mpv 原生一致"的必要条件。
/// 见 WaylandColorFeedback.h。
int declareHdr10Surface(QQuickWindow *window);

/// 把窗口/swapchain/色彩空间声明切到 mode（运行期，含 Qt swapchain 重建）。
/// 步骤：属性（供 Qt 将来重建）× QSurfaceFormat::colorSpace + 重声明 ×
/// `QRhiSwapChain::setFormat` + `newCompatibleRenderPassDescriptor` +
/// `createOrResize()`。**调用方随后必须重建自己的渲染通道**（composite
/// pipeline / rtImage 格式）——见 installPolicy 的 rebuildRenderPath 钩子。
/// 返回 false 时 swapchain 保持原状（`err` 给出原因，调用方应保持 SDR）。
bool applyMode(QQuickWindow *window, Mode mode, QString *err);

/// 自动策略的应用侧钩子（都用 GUI 线程回调）。
struct ApplyHooks {
    /// 目标模式变化后重建渲染链路（composite pipeline + rtImage 格式）。
    std::function<void(Mode)> rebuildRenderPath;
    /// 更新 mpv 的 FBO 色彩空间目标（亮度取 DisplayInfo，参考白口径）。
    std::function<void(Mode, const DisplayInfo &)> updateMpvTarget;
    /// HDR 状态可能变化（模式/显示能力/内容元数据）→ 刷新 OSD 等展示层。
    /// 幂等、无参数；展示内容用 `statusText()` 取。
    std::function<void()> statusChanged;
};

/// 安装自动策略：显示能力（surface preferred，异步）× 内容 HDR（setContentHdr）
/// × 策略 → 目标模式；变化时自动 applyMode + 调钩子。幂等。
/// `initialMode` = 应用启动时已生效的模式（show 前设的），用于跳过无谓的首次应用。
void installPolicy(QQuickWindow *window, Policy policy, Mode initialMode,
                   ApplyHooks hooks);

/// 内容是否 HDR（来自 mpv video-params/gamma ∈ {pq, hlg}）。触发重新决策。
void setContentHdr(bool hdr);

/// 当前决策出的目标模式（诊断/OSD 用）。
Mode targetMode();

/// 当前 HDR 状态的一行文本（OSD 用；**不翻译**，与 OSD 其余状态段一致）。
/// 形如：
///   `HDR10 · BT.2020/PQ · display 400nit · content 758nit`（正在 HDR10 上屏）
///   `SDR · content HDR not displayed (display SDR)`（HDR 片源但未上屏 + 原因）
///   `SDR · content SDR`（SDR 片源）
QString statusText();

/// 只读探测：不改变任何窗口/swapchain 状态。必须在场景图就绪后调用
/// （afterRendering 第一帧），否则 swapchain 还没绑定窗口 → probed=false。
Capabilities probeSwapchain(QQuickWindow *window);

/// 接线：首帧探测一次并打日志；窗口换屏时重新探测（per-surface 能力）。
/// 零行为变更（只读 + 日志）——HDR 格式的选择/应用是后续步骤。
void installCapabilityProbe(QQuickWindow *window);

/// 首帧后核对 Qt 实际使用的 swapchain 格式是否 == expected，只核对一次：
/// 一致 → INFO；不一致 → ERR（强制模式**不自动回退**，但必须可见）。
void installSwapchainVerification(QQuickWindow *window, Mode expected);

} // namespace hdr
