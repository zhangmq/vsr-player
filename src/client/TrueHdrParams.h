#pragma once

// TrueHDR（SDR→HDR10 上变换）参数的**合法区间与默认值**——客户端唯一来源。
//
// 数值来源 = SDK 自身的参数文档串（`strings libnvVFXTrueHDR.so`，逐条照抄）：
//   Contrast     (unsigned int) - contrast tunable,   default 100, range [0..200]
//   Saturation   (unsigned int) - saturation tunable, default 100, range [0..200]
//   MiddleGray   (unsigned int) - middle-grey reference, default 50, range [10..100]
//   Luminance    (unsigned int) - target HDR display peak luminance in nits,
//                                 default 650, range [400..2000]
//   DebandingOff (unsigned int) - 1 = skip the DL-Debander pass, default 0, [0 or 1]
//
// ⚠ SDK **没有** preset/profile 概念（全库 grep preset|profile|recommend 零命中，
//   导出符号里也没有对应 API）——所以 UI 的 "reset" 目标只能是这里的 SDK 默认值。
// ⚠ 同一组数值在 C 侧镜像一份：`src/mpv/video/filter/vf_truehdr.c` 的选项表
//   （跨构建目标无法共享头文件：libmpv 与 client 分开编译）——**改动必须两处同步**。

namespace truehdr_params {

inline constexpr int kLuminanceMin      = 400;
inline constexpr int kLuminanceMax      = 2000;
inline constexpr int kLuminanceDefault  = 650;

inline constexpr int kSaturationMin     = 0;
inline constexpr int kSaturationMax     = 200;
inline constexpr int kSaturationDefault = 100;

inline constexpr int kContrastMin       = 0;
inline constexpr int kContrastMax       = 200;
inline constexpr int kContrastDefault   = 100;

inline constexpr int kMiddleGrayMin     = 10;
inline constexpr int kMiddleGrayMax     = 100;
inline constexpr int kMiddleGrayDefault = 50;

/// 抗色带（DebandingOff 的**反相**语义：UI 用"开/关"，SDK 用 0=开/1=跳过）。
/// 默认开 = SDK 默认（0）——同时满足项目铁律 6（条件触发的处理项常开）。
inline constexpr bool kDebandingDefault = true;

inline int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace truehdr_params
