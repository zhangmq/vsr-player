#pragma once

#include "yuv_to_rgba.h"

// ── NvCVImage（nvCVImage.h 的结构布局，C 兼容重定义）──────────────────
// 图像缓冲为 per-context（多实例共存——退役队列延迟销毁方案）：每个
// vsr_context 持有自己的 in/out/tmp 缓冲，SDK 引擎的 SetImage 引用
// 各实例独立的 pixels 地址，销毁旧实例不影响新实例。

typedef struct NvCVImage {
    unsigned int width, height;
    int pitch;
    int pixelFormat, componentType;
    unsigned char pixelBytes, componentBytes, numComponents;
    unsigned char planar, gpuMem, colorspace;
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void*);
    unsigned long long bufferBytes;
} NvCVImage;

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
    // ── per-context 图像缓冲（引擎生命周期内随尺寸按需重建）
    struct NvCVImage in_img, out_img, tmp_img;
};

bool vsr_init(struct vsr_context *c, int in_w, int in_h,
              int out_w, int out_h, int quality, CUstream stream);
bool vsr_warmup(struct vsr_context *c, CUstream stream);
// ── 轻量热更新（引擎常驻）──────────────────────────────────────────────
// quality：Load 后 SetU32 更新。仅输出尺寸（输入不变）：SetImage 换
// 输出缓冲（vfx_outchange_test + 播放器帧号 dump：PSNR 999dB 逐字节
// 一致）——全屏切换免引擎重建。输入变化：重建（真实播放器换文件时
// mpv 重建整个 filter 链，引擎必然重建 + warmup）。
bool vsr_set_quality(struct vsr_context *c, int quality);
bool vsr_set_output(struct vsr_context *c, int out_w, int out_h);
// Caller writes RGBA into c->in_pixels before calling vsr_process.
bool vsr_process(struct vsr_context *c, CUstream stream,
                 void **output_ptr, int *out_w, int *out_h, int *out_pitch);
// 销毁引擎；销毁前等待 stream 在途工作完成（防 GPU 写已释放缓冲）。
void vsr_destroy(struct vsr_context *c);
