// src/mpv/video/filter/yuv_to_rgba.h
#pragma once

#include <cuda.h>
#include <stdbool.h>

enum yuv_matrix {
    YUV_MATRIX_BT601,
    YUV_MATRIX_BT709,
    YUV_MATRIX_BT2020,
};

enum yuv_range {
    YUV_RANGE_LIMITED,
    YUV_RANGE_FULL,
};

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
