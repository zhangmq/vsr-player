#pragma once

#include "yuv_to_rgba.h"

// ── VSR processor ─────────────────────────────────────────────────────

struct vsr_context {
    void   *handle;       // NvVFX_Handle
    void   *in_pixels;    // allocated RGBA U8 GPU buffer
    void   *out_pixels;   // allocated RGBA U8 GPU buffer
    void   *tmp_pixels;   // temp transfer buffer
    int     in_pitch, out_pitch, tmp_pitch;
    CUstream stream;
    bool    own_stream;
    bool    ready;
    int     in_w, in_h, out_w, out_h;
    int     quality;
};

bool vsr_init(struct vsr_context *c, int in_w, int in_h,
              int out_w, int out_h, int quality, CUstream stream);
bool vsr_warmup(struct vsr_context *c, CUstream stream);
// ── 轻量热更新（仅 quality）────────────────────────────────────────────
// SDK 实验结论（vfx_test5）：SetImage 换输入/输出尺寸输出错乱（引擎
// 内部 tile 切分与 Load 时尺寸绑定）→ 尺寸变化必须重建管线
// （vsr_destroy + vsr_init）；仅 QualityLevel 可 Load 后 SetU32 更新。
bool vsr_set_quality(struct vsr_context *c, int quality);
// Caller writes RGBA into c->in_pixels before calling vsr_process.
bool vsr_process(struct vsr_context *c, CUstream stream,
                 void **output_ptr, int *out_w, int *out_h, int *out_pitch);
// 销毁引擎；销毁前等待 stream 在途工作完成（防 GPU 写已释放缓冲）。
void vsr_destroy(struct vsr_context *c);
