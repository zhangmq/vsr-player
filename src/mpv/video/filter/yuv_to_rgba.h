// src/mpv/video/filter/yuv_to_rgba.h
#pragma once

#include <cuda.h>
#include <stdbool.h>
#include "mp_image.h"    // struct mp_image_params
#include "csputils.h"    // pl_color_system / pl_color_levels / mp_csp_guess_colorspace

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

struct yuv_to_rgba {
    CUmodule   module;
    CUfunction kernel;
    bool       ready;
};

bool yuv_to_rgba_init(struct yuv_to_rgba *c, int bit_depth,
                       enum yuv_matrix matrix, enum yuv_range range);
bool yuv_to_rgba_convert(struct yuv_to_rgba *c,
                          void *y_plane,   int y_pitch,
                          void *uv_plane,  int uv_pitch,
                          int width, int height,
                          void *rgba_output, int rgba_pitch,
                          CUstream stream);
void yuv_to_rgba_destroy(struct yuv_to_rgba *c);
