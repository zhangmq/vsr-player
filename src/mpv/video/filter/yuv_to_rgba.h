// src/mpv/video/filter/yuv_to_rgba.h
#pragma once

#include <cuda.h>
#include <stdbool.h>
#include "video/img_format.h"  // mp_get_regular_imgfmt / IMGFMT_*
#include "video/mp_image.h"    // struct mp_image_params
#include "video/csputils.h"    // pl_color_system / pl_color_levels / mp_csp_guess_colorspace

enum yuv_matrix {
    YUV_MATRIX_BT601,
    YUV_MATRIX_BT709,
    YUV_MATRIX_BT2020,
};

enum yuv_range {
    YUV_RANGE_LIMITED,
    YUV_RANGE_FULL,
};

// ── Color params → yuv enum（单一事实源，vf_vsr/vf_rife 共用）────────
static inline enum yuv_matrix matrix_from_repr(struct mp_image_params *p)
{
    if (p->repr.sys == PL_COLOR_SYSTEM_UNKNOWN) {
        enum pl_color_system guessed = mp_csp_guess_colorspace(p->w, p->h);
        switch (guessed) {
        case PL_COLOR_SYSTEM_BT_709:     return YUV_MATRIX_BT709;
        case PL_COLOR_SYSTEM_BT_2020_NC:
        case PL_COLOR_SYSTEM_BT_2020_C:  return YUV_MATRIX_BT2020;
        default:                         return YUV_MATRIX_BT601;
        }
    }
    switch (p->repr.sys) {
    case PL_COLOR_SYSTEM_BT_709:     return YUV_MATRIX_BT709;
    case PL_COLOR_SYSTEM_BT_2020_NC:
    case PL_COLOR_SYSTEM_BT_2020_C:
    case PL_COLOR_SYSTEM_BT_2100_PQ:
    case PL_COLOR_SYSTEM_BT_2100_HLG: return YUV_MATRIX_BT2020;
    default:                          return YUV_MATRIX_BT601;
    }
}

static inline enum yuv_range range_from_levels(enum pl_color_levels levels)
{
    return (levels == PL_COLOR_LEVELS_FULL) ? YUV_RANGE_FULL : YUV_RANGE_LIMITED;
}

// ── 10-bit 通路（VFX RGB10A2）：位深 ↔ mpv 像素格式（单一事实源）──────
// 打包字为**小端 u32**：R 在 bit0-9、G 在 bit10-19、B 在 bit20-29、
// A 在 bit30-31。实测依据（build/scratch/vsr10_probe.c，用 NVIDIA 自家的
// NvCVImage_Transfer，不靠猜）：
//   RGBA8(255,0,0) -> RGB10A2 得到 word=0xC00003FF（低 10 位 = R）
//   RGB10A2(word=0x000003FF) -> RGBA8 得到 (255,0,0)
// 该位序 = mpv **IMGFMT_X2BGR10**（"2pad 10b 10g 10r"，MSB→LSB，即 R 在
// 最低位）= VK_FORMAT_A2B10G10R10_UNORM_PACK32 = FFmpeg AV_PIX_FMT_X2BGR10。
// ⚠ 不是 IMGFMT_X2RGB10（那是 "2pad 10r 10g 10b"，R 在最高位，对应
// VK_FORMAT_A2R10G10B10）——VN 里名字只差一个字母，选错会 R/B 互换。
// 因此滤镜链用 X2BGR10 承载 VFX 的 RGB10A2 数据，不需要位重排。
#define VFX_BITS_8   8
#define VFX_BITS_10  10

/// mpv 帧 subfmt → 通路位深（8/10）。
/// ⚠ 不能用 imgfmt_desc.bpp[0]：那是**整像素位宽**（RGBA=32、NV12=8、
/// P010=16），拿它比 8 会把 RGBA 判成 10-bit（实测：SDR 8-bit 内容
/// 走 VFG 时被误判 10-bit，引擎按 RGB10A2 解释 RGBA 字节 → 输出花屏）。
/// 正确口径 = 单个**分量**的存储字节数：
///   NV12/RGBA → 1 字节 → 8-bit；P010/P016 → 2 字节 → 10-bit；
///   X2BGR10 是 packed 特殊格式（mp_get_regular_imgfmt 不支持 10bit/8
///   不整除）→ 单独判定。
static inline int vfx_bits_from_imgfmt(int imgfmt)
{
    if (imgfmt == IMGFMT_X2BGR10 || imgfmt == IMGFMT_X2RGB10)
        return VFX_BITS_10;
    struct mp_regular_imgfmt reg;
    if (mp_get_regular_imgfmt(&reg, imgfmt))
        return reg.component_size > 1 ? VFX_BITS_10 : VFX_BITS_8;
    return VFX_BITS_8;
}

/// 通路位深 → 输出帧 RGB 格式。
static inline int vfx_imgfmt_from_bits(int bits)
{
    return bits == VFX_BITS_10 ? IMGFMT_X2BGR10 : IMGFMT_RGBA;
}

/// 该 subfmt 是否已是 RGB（RGBA/BGRA/X2RGB10）——是则直接 D2D 拷贝，
/// 不当 YUV 做矩阵转换（否则会把 R 平面当 Y、G 平面当色度读）。
static inline bool vfx_imgfmt_is_rgb(int imgfmt)
{
    return imgfmt == IMGFMT_RGBA || imgfmt == IMGFMT_BGRA ||
           imgfmt == IMGFMT_X2BGR10 || imgfmt == IMGFMT_X2RGB10;
}

struct yuv_to_rgba {
    CUmodule   module;
    CUfunction kernel;
    bool       ready;
};

/// out_bits：输出像素格式，8 = RGBA U8（4 字节/px），10 = 打包 RGB10A2
/// （同样 4 字节/px，位序见上）。transfer 曲线**不参与转换**（与历史行为
/// 一致：PQ/HLG 码值原样搬运，tonemap 交给 mpv 的 VO）。
bool yuv_to_rgba_init(struct yuv_to_rgba *c, int bit_depth,
                       enum yuv_matrix matrix, enum yuv_range range,
                       int out_bits);
bool yuv_to_rgba_convert(struct yuv_to_rgba *c,
                          void *y_plane,   int y_pitch,
                          void *uv_plane,  int uv_pitch,
                          int width, int height,
                          void *rgba_output, int rgba_pitch,
                          CUstream stream);
void yuv_to_rgba_destroy(struct yuv_to_rgba *c);
