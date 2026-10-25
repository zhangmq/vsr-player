#pragma once
#include <cstdint>

/// Composite 片元着色器的 push constants（24 B）。字段顺序/类型必须与
/// shaders/composite.frag 的 `layout(push_constant)` 一致。
struct StereoCompositeParams {
    int32_t active = 0;   ///< 0=普通 2D；1=3D 映射
    int32_t layout = 0;   ///< 0=SBS，1=上下
    int32_t swap = 0;     ///< 交换左右眼
    int32_t halfMode = 0; ///< 0=全（全宽/全高），1=半（半宽/半高）
    float   srcAspect = 1.0f;  ///< 源视频整帧 W/H（未 letterbox 前）
    float   outAspect = 1.0f;  ///< 输出窗口 W/H
};
