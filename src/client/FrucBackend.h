#pragma once
// ── FRUC（插帧）后端编译期分支 ────────────────────────────────────────
// 后端选择的唯一事实源：meson 选项 `fruc_backend`（仓库根 meson.options，
// 默认 vfg）→ src/client/meson.build 定义 FRUC_BACKEND_VFG /
// FRUC_BACKEND_RIFE 二选一宏。本文件集中所有与后端耦合的名字与语义，
// 其余客户端代码只经此处取用：
//   * 滤镜名（vf 串 label、vf-command 目标、mpv 日志模块名）
//   * 状态行前缀（滤镜 → 客户端 OSD，冻结接口）
//   * CLI --fruc 合法值（帮助文本 + 解析）与 UI 档位模型（frucModel）
//   * --fruc-order（插帧与超分的先后）是否可切换
//   * --fruc-quality（插帧质量档 = VFG 的 mode）合法值、UI 档位模型
//     （frucQualityModel）与默认值是否可选
// 语义差异（冻结决策）：
//   VFG （NVIDIA VFX SDK 1.3 VideoFrameGeneration）＝倍率语义 fps=2|3|4|off，
//        链序默认后置 hwup → vsr → vfg → VO（--fruc-order pre 可前置）；
//        另有质量档 mode=low|medium|high（--fruc-quality，热切换）；
//   RIFE（TensorRT）＝目标帧率语义 fps=40|48|60|off（2/3/4 仅 benchmark
//        倍率硬跑），链序固定 hwup → rife → vsr → VO（源分辨率插帧）；
//        无质量档（--fruc-quality 接受但忽略，同 --fruc-order）。
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

// QT_TRANSLATE_NOOP：档位标签在 C++ 侧经 tr() 翻译（context =
// PlayerViewModel），宏本身在编译期就是原字符串，仅给 lupdate 留提取标记。
#include <QtCore/qttranslation.h>

#if defined(FRUC_BACKEND_VFG) && defined(FRUC_BACKEND_RIFE)
#error "FRUC_BACKEND_VFG / FRUC_BACKEND_RIFE 互斥（meson option fruc_backend 单选）"
#endif
#if !defined(FRUC_BACKEND_VFG) && !defined(FRUC_BACKEND_RIFE)
#error "未定义 FRUC 后端宏：src/client/meson.build 应传 -DFRUC_BACKEND_VFG|_RIFE"
#endif

namespace fruc {

/// UI 档位（PlayerViewModel::frucModel → QualityPopup 的 label/value）。
/// value：-1 = 直通（off）；>0 = 倍率（VFG）/ 目标帧率（RIFE）。
/// label 为 tr() 源串（QML 的 qsTr 迁到 C++，见 PlayerViewModel::frucModel）；
/// QT_TRANSLATE_NOOP 的 scope 必须是 PlayerViewModel（tr() 的 context）——
/// .ts 两个分支的标签都手工登记（编译期分支，lupdate 只能看到当前分支）。
struct Preset {
    int value;
    const char *label;
};

/// 插帧质量档（--fruc-quality → VFG VideoFrameGeneration 的 Mode）。
/// value：SDK Mode 枚举 0=low / 1=medium / 2=high；name：vf 串、vf-command、
/// CLI 与 RPC 的取值（三者共用同一名字来源，禁止手写第二次）；label 为
/// tr() 源串（同 Preset，QT_TRANSLATE_NOOP scope = PlayerViewModel）。
struct ModePreset {
    int value;
    const char *label;
    const char *name;
};

#ifdef FRUC_BACKEND_VFG
inline constexpr bool kIsVfg = true;
/// --fruc-order 仅 VFG 有意义（RIFE 链序固定：插帧在超分前、源分辨率处理）。
inline constexpr bool kOrderSelectable = true;
/// --fruc-quality（VFG VideoFrameGeneration 的 Mode）仅 VFG 有意义
/// （RIFE 无质量档：接受但忽略，同 --fruc-order）。
inline constexpr bool kModeSelectable = true;
/// 滤镜名（冻结接口）："vfg"。同时是 mpv 日志模块名——filters/filter.c 用
/// mp_log_new(f, ..., params->info->name) → msg-level 的通道名即滤镜名。
#define FRUC_FILTER_NAME "vfg"
/// 状态行前缀（冻结接口，滤镜侧产出，客户端提取入 OSD）。
inline constexpr const char *kStatusPrefix = "vfg-status:";
inline constexpr Preset kPresets[] = {
    { -1, QT_TRANSLATE_NOOP("PlayerViewModel", "Off") },
    { 2, QT_TRANSLATE_NOOP("PlayerViewModel", "×2") },
    { 3, QT_TRANSLATE_NOOP("PlayerViewModel", "×3") },
    { 4, QT_TRANSLATE_NOOP("PlayerViewModel", "×4") },
};
/// CLI --fruc 合法值（VFG：倍率即正常语义，无 benchmark 特殊档）。
inline constexpr const char *kCliValues[] = { "off", "2", "3", "4" };
#else
inline constexpr bool kIsVfg = false;
inline constexpr bool kOrderSelectable = false;
/// RIFE 无质量档（--fruc-quality 接受但忽略 + 警告，同 --fruc-order）。
inline constexpr bool kModeSelectable = false;
#define FRUC_FILTER_NAME "rife"
/// RIFE 侧维持既有前缀（fruc-status，客户端 OSD 现有行为）。
inline constexpr const char *kStatusPrefix = "fruc-status:";
inline constexpr Preset kPresets[] = {
    { -1, QT_TRANSLATE_NOOP("PlayerViewModel", "Off") },
    { 40, QT_TRANSLATE_NOOP("PlayerViewModel", "40 fps") },
    { 48, QT_TRANSLATE_NOOP("PlayerViewModel", "48 fps") },
    { 60, QT_TRANSLATE_NOOP("PlayerViewModel", "60 fps") },
};
/// CLI --fruc 合法值（RIFE：2/3/4 = benchmark 倍率硬跑，40/48/60 = 目标帧率）。
inline constexpr const char *kCliValues[] = { "off", "40", "48", "60", "2", "3", "4" };
#endif

/// 滤镜名（vf 串 label / vf-command 目标 / 日志模块名，同源 FRUC_FILTER_NAME）。
inline constexpr const char *kFilterName = FRUC_FILTER_NAME;
/// benchmark 的 msg-level：全静默 + 只放行插帧滤镜的 status 通道
/// （状态行供 OSD；字符串由滤镜名拼出，不手写第二次）。
inline constexpr const char *kMsgLevelBenchmark = "all=no," FRUC_FILTER_NAME "=status";


inline constexpr std::size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
inline constexpr std::size_t kCliValueCount = sizeof(kCliValues) / sizeof(kCliValues[0]);

/// --fruc-order 取值（仅 kOrderSelectable 分支生效）；默认后置。
inline constexpr const char *kOrderDefault = "post";

// ── 插帧质量档（VFG mode；--fruc-quality / set-fruc-quality）──────────
// 表本身与后端分支无关（VFG 的 SDK Mode 枚举），是否可选由各分支的
// kModeSelectable 决定：RIFE 分支仍能解析/打印，但客户端不发送、界面隐藏。
inline constexpr ModePreset kModePresets[] = {
    { 0, QT_TRANSLATE_NOOP("PlayerViewModel", "Low"),    "low" },
    { 1, QT_TRANSLATE_NOOP("PlayerViewModel", "Medium"), "medium" },
    { 2, QT_TRANSLATE_NOOP("PlayerViewModel", "High"),   "high" },
};
/// 默认档 = medium（mode=1）。
/// 为什么是 medium（供后人重做决定）：播放器内实测 4K 输出（1080p ×
/// scale=2）上 VFG cost MEDIUM 3.8ms vs HIGH 13.2ms，HIGH 的帧时间 p90 从
/// 16.5ms 恶化到 24.1ms（另一次同配置运行累积 9 个丢帧）；而 MEDIUM 在原生
/// 4K 素材上 2.7ms / 48fps / 0 丢帧已足够，HIGH 换来的画质差仅为 4K mid/blend
/// 高频 0.918 vs 0.956（目检未见可见软化）。⚠️ 2026-10-16 决定：`vf_vfg` 的
/// `mode=auto` **已彻底移除**（不要自动档）——档位只有 low/medium/high 三档、
/// 默认 medium，客户端总是发显式值，滤镜侧不再有任何"自动挑档"语义。
inline constexpr int kModeDefault = 1;
inline constexpr std::size_t kModePresetCount =
    sizeof(kModePresets) / sizeof(kModePresets[0]);

/// 质量档合法性（UI setFrucQuality / RPC set-fruc-quality / loadSettings）。
inline bool isValidMode(int v)
{
    for (std::size_t i = 0; i < kModePresetCount; ++i)
        if (kModePresets[i].value == v)
            return true;
    return false;
}

/// 质量档 → 取值字符串（vf 串 / vf-command / CLI / RPC 单一来源）。
/// 非法值回落默认档（防御性：setter 已校验、loadSettings 已清洗，此处只为
/// 永不产出非法 vf 串）。
inline const char *modeName(int v)
{
    std::size_t def = 0;
    for (std::size_t i = 0; i < kModePresetCount; ++i) {
        if (kModePresets[i].value == v)
            return kModePresets[i].name;
        if (kModePresets[i].value == kModeDefault)
            def = i;
    }
    return kModePresets[def].name;
}

/// --fruc-quality / RPC set-fruc-quality 解析：合法 → true 并写 *out；
/// 空/非法 → false。
inline bool parseMode(const std::string &s, int *out)
{
    for (std::size_t i = 0; i < kModePresetCount; ++i) {
        if (s != kModePresets[i].name)
            continue;
        *out = kModePresets[i].value;
        return true;
    }
    return false;
}

/// CLI/RPC 合法值列表文本（单一事实源 = kModePresets，同 cliValuesHelp）。
inline std::string modeValuesHelp()
{
    std::string s;
    for (std::size_t i = 0; i < kModePresetCount; ++i) {
        if (i)
            s += '|';
        s += kModePresets[i].name;
    }
    return s;
}

/// 正常档位合法性（UI setFrucFps / RPC set-fruc）。
inline bool isValidPreset(int v)
{
    for (std::size_t i = 0; i < kPresetCount; ++i)
        if (kPresets[i].value == v)
            return true;
    return false;
}

/// 持久化值清洗（loadSettings：手改配置/上一后端残留 → off）。
/// RIFE 分支 = 改动前的判定集合 **补上 48**（{ -1, 30, 40, 48, 60 }）：48 是
/// QualityPopup 可达档位，原 sanitizer 漏列它 → 用户选 48 fps 后重启被清成
/// off（用户已确认修）。保留 30 是为了对遗留配置零行为变化：UI/CLI 早已不再
/// 提供 30，但旧版本持久化过它的用户不该被清档。
/// VFG 分支只认倍率档 { -1, 2, 3, 4 }（RIFE 时代的 40/48/60 残留 → off）。
inline bool isValidPersisted(int v)
{
#ifdef FRUC_BACKEND_VFG
    return isValidPreset(v);
#else
    return v == -1 || v == 30 || v == 40 || v == 48 || v == 60;
#endif
}

/// CLI --fruc 解析：合法 → true 并写 *out（"off" → -1）；空/非法 → false。
inline bool parseCli(const std::string &s, int *out)
{
    for (std::size_t i = 0; i < kCliValueCount; ++i) {
        if (s != kCliValues[i])
            continue;
        *out = (strcmp(kCliValues[i], "off") == 0) ? -1 : atoi(kCliValues[i]);
        return true;
    }
    return false;
}

/// CLI 合法值列表文本（帮助文本单一事实源 = kCliValues）。
inline std::string cliValuesHelp()
{
    std::string s;
    for (std::size_t i = 0; i < kCliValueCount; ++i) {
        if (i)
            s += '|';
        s += kCliValues[i];
    }
    return s;
}

/// RPC 档位提示文本（单一事实源 = kPresets）。
inline std::string presetValuesHelp()
{
    std::string s;
    for (std::size_t i = 0; i < kPresetCount; ++i) {
        if (i)
            s += '|';
        s += std::to_string(kPresets[i].value);
    }
    return s;
}

} // namespace fruc
