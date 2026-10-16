#include "vsr_internal.h"
#include "vfx_loader.h"

#include <cuda.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


// ── Types from nvCVImage.h / nvVideoEffects.h (C-compatible) ────────────
// struct NvCVImage 与 NVCV_* 常量定义见 vfx_loader.h（VSR/VFG 共用）

#define NVVFX_INPUT_IMAGE   "SrcImage0"
#define NVVFX_OUTPUT_IMAGE  "DstImage0"
#define NVVFX_CUDA_STREAM   "CudaStream"

// 图像缓冲为 per-context（vsr_context.in_img/out_img/tmp_img）——引擎
// 生命周期内缓冲随尺寸按需重建（vsr_set_output），SDK 引擎的 SetImage
// 引用各实例自身的 pixels 地址。

// ── 图像缓冲按需分配（尺寸变化时重建，旧缓冲先释放）──────────────────
// 单一分配入口：vsr_init 与 vsr_set_output/set_input 共用，杜绝
// "旧缓冲未释放即被覆盖"的泄漏路径（旧实现 scale 热更新会 memset
// 丢弃 g_out_img 而未 Dealloc）。
static bool ensure_img(NvCVImage *img, int w, int h, const char *tag) {
    if (img->pixels && img->width == (unsigned)w && img->height == (unsigned)h)
        return true;
    if (img->pixels) {
        pfn_NvCVImage_Dealloc(img);
        memset(img, 0, sizeof(*img));
    }
    int ar = pfn_NvCVImage_Alloc(img, (unsigned)w, (unsigned)h,
                                 NVCV_RGBA, NVCV_U8,
                                 NVCV_CHUNKY, NVCV_GPU, 32);
    if (ar != 0) {
        fprintf(stderr, "VSR: Alloc(%s %dx%d) failed (%d)\n", tag, w, h, ar);
        memset(img, 0, sizeof(*img));
        return false;
    }
    return true;
}

// ── vsr_init ─────────────────────────────────────────────────────────────

bool vsr_init(struct vsr_context *c, int in_w, int in_h,
              int out_w, int out_h, int quality, CUstream stream) {
    if (!vfx_load_libraries()) return false;

    memset(c, 0, sizeof(*c));
    c->in_w = in_w;
    c->in_h = in_h;
    c->out_w = out_w;
    c->out_h = out_h;
    c->quality = quality;

    // Create effect — responds to "VideoSuperRes" (not "SuperRes")
    int ret = pfn_NvVFX_CreateEffect("VideoSuperRes", &c->handle);
    if (ret != 0) {
        fprintf(stderr, "VSR: CreateEffect(VideoSuperRes) failed (%d)\n", ret);
        return false;
    }

    // QualityLevel (not "Strength")
    pfn_NvVFX_SetU32(c->handle, "QualityLevel", (unsigned int)quality);
    {
        bool denoise = (in_w == out_w && in_h == out_h);
        fprintf(stderr, "VSR: [%s] %dx%d->%dx%d quality=%d %s\n",
                denoise ? "DENOISE" : "UPSCALE",
                in_w, in_h, out_w, out_h, quality,
                denoise ? "(1:1 denoising)" : "(AI super-resolution)");
    }

    // ── Output image: RGBA U8 chunky GPU ──────────────────────────────────
    if (!ensure_img(&c->out_img, out_w, out_h, "output")) {
        vsr_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, NVVFX_OUTPUT_IMAGE, &c->out_img);
    fprintf(stderr, "VSR: DstImage0 RGBA U8 -> %d\n", ret);
    if (ret != 0) {
        vsr_destroy(c);
        return false;
    }
    c->out_pixels = c->out_img.pixels;
    c->out_pitch = (int)c->out_img.pitch;

    // ── Input image: RGBA U8 chunky GPU ───────────────────────────────────
    if (!ensure_img(&c->in_img, in_w, in_h, "input")) {
        vsr_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, NVVFX_INPUT_IMAGE, &c->in_img);
    if (ret != 0) {
        fprintf(stderr, "VSR: SetImage(input RGBA U8) failed (%d)\n", ret);
        vsr_destroy(c);
        return false;
    }
    c->in_pixels = c->in_img.pixels;
    c->in_pitch = (int)c->in_img.pitch;

    // ── Temp buffer for NvCVImage_Transfer ────────────────────────────────
    {
        int max_dim = (out_w > in_w) ? out_w : in_w;
        int max_h = (out_h > in_h) ? out_h : in_h;
        if (!ensure_img(&c->tmp_img, max_dim, max_h, "temp"))
            memset(&c->tmp_img, 0, sizeof(c->tmp_img));   // 非致命
    }
    c->tmp_pixels = c->tmp_img.pixels;
    c->tmp_pitch = (int)c->tmp_img.pitch;

    // ── CUDA stream ───────────────────────────────────────────────────────
    // 交给引擎的必须是**阻塞流**（官方 "Setting Up the CUDA Stream"：
    // cudaStreamCreate() 或 NvVFX_CudaStreamCreate() 建流后 NvVFX_SetCudaStream；
    // 后者反汇编可见就是一句 cudaStreamCreate → 默认 flags = 阻塞流）。
    // VFX 1.3 内部有固定挂在 legacy 默认流上的环节（1.1 无此机制），非阻塞流与
    // legacy 流**没有隐含顺序** → 引擎读到尚未落地的输入，表现为每个输出帧装的
    // 是上一帧（外加非确定性重复/跳源帧）。实测：build/scratch/probe/
    // BLOCKING_REPORT.md（阻塞流 offset 0，非阻塞流 1~2）。
    // ⚠️ 输出必须在本流上读回；换别的流读需要自行同步。
    if (stream) {
        c->stream = stream;
        c->own_stream = false;
    } else {
        cuStreamCreate(&c->stream, CU_STREAM_DEFAULT);
        c->own_stream = true;
    }
    pfn_NvVFX_SetCudaStream(c->handle, NVVFX_CUDA_STREAM, c->stream);

    // ── Load model ────────────────────────────────────────────────────────
    ret = pfn_NvVFX_Load(c->handle);
    if (ret != 0) {
        fprintf(stderr, "VSR: Load failed (%d)\n", ret);
        vsr_destroy(c);
        return false;
    }

    c->ready = true;
    fprintf(stderr, "VSR: init ok\n");
    return true;
}

// ── vsr_warmup ────────────────────────────────────────────────────────────
// Must be called after vsr_init, before the first vsr_process.
// Blocks until complete (~30ms for 3 random frames on the given stream).

bool vsr_warmup(struct vsr_context *c, CUstream stream)
{
    // Temporarily set stream for warmup, restore after
    CUstream saved = c->stream;
    c->stream = stream;

    size_t in_bytes = (size_t)c->in_pitch * c->in_h;
    unsigned char *random_rgba = (unsigned char*)malloc(in_bytes);
    if (!random_rgba) {
        fprintf(stderr, "VSR: warmup malloc failed\n");
        c->stream = saved;
        return false;
    }
    unsigned int seed = 42;
    for (size_t i = 0; i < in_bytes; i += 4) {
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 0] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 1] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 2] = (unsigned char)((seed >> 16) & 0xFF);
        random_rgba[i + 3] = 255;
    }
    cuMemcpyHtoD((CUdeviceptr)c->in_pixels, random_rgba, in_bytes);
    free(random_rgba);

    for (int wu = 0; wu < 3; wu++) {
        int wr = pfn_NvVFX_Run(c->handle, 0);
        if (wr != 0) {
            fprintf(stderr, "VSR: warmup[%d] Run -> %d\n", wu, wr);
            c->stream = saved;
            return false;
        }
    }
    c->stream = saved;
    fprintf(stderr, "VSR: warmup complete (3 frames with random data)\n");
    return true;
}

// ── vsr_process ──────────────────────────────────────────────────────────
// Caller must have written RGBA into c->in_pixels (HtoD or D2D on stream).
// 引擎跑在 c->stream（阻塞流，见 vsr_init）上，调用方的写入与读回也在同一条
// 流上 → 靠 CUDA 流内顺序即可（写完才读、跑完才拷）。
// **输出必须在同一条流上读回**；跨流读回需要自行同步。

bool vsr_process(struct vsr_context *c, CUstream stream,
                 void **output_ptr, int *out_w, int *out_h, int *out_pitch) {
    if (!c->ready) return false;

    int ret = pfn_NvVFX_Run(c->handle, 0);
    if (ret != 0) {
        fprintf(stderr, "VSR: Run failed (%d)\n", ret);
        return false;
    }

    ret = pfn_NvVFX_GetImage(c->handle, NVVFX_OUTPUT_IMAGE, &c->out_img);
    if (ret != 0) {
        fprintf(stderr, "VSR: GetImage(output) failed (%d)\n", ret);
        return false;
    }

    *output_ptr = c->out_img.pixels;
    *out_w = (int)c->out_img.width;
    *out_h = (int)c->out_img.height;
    if (out_pitch) *out_pitch = (int)c->out_img.pitch;

    return true;
}

// ── 轻量热更新（引擎常驻）──────────────────────────────────────────────
// SDK 实验结论（vfx_test5）：
//   - SetImage 换输入/输出尺寸 → 输出错乱（PSNR 7.4dB）——引擎内部
//     tile 切分与 Load 时尺寸绑定，SetImage 仅更新元数据。尺寸变化
//     必须重建管线（vsr_destroy + vsr_init）。
//   - SetU32 改 QualityLevel → 生效（q1 vs q3 输出 PSNR 44.8dB 结构化
//     差异）——quality 可轻量更新，引擎常驻。
// 旧实现每次热更新全量重建引擎（~1s Load 延迟 + 销毁竞态 + scale 路径
// 旧引擎泄漏）；上一版轻量实现（SetImage 换尺寸）输出错乱——均废弃。

// ── 轻量热更新：尺寸变化（引擎常驻）──────────────────────────────────
// 实验（vfx_outchange_test / vfx_inchange_test）：引擎 Load 后 SetImage
// 换输出/输入缓冲，输出与直接加载新尺寸的引擎逐字节一致（PSNR 999dB）
// ——SDK 引擎在 SetImage 时重新适配（含 tile 切分），无需重建。尺寸
// 变化（输入或输出）均走此路径，绕开重建的 destroy/Load 挂死点
// （Xid 109）。

bool vsr_set_output(struct vsr_context *c, int out_w, int out_h) {
    if (!c->handle || !c->ready) return false;
    if (out_w == c->out_w && out_h == c->out_h)
        return true;
    if (!ensure_img(&c->out_img, out_w, out_h, "output")) {
        fprintf(stderr, "VSR: SetOutput alloc %dx%d failed\n", out_w, out_h);
        return false;
    }
    int ret = pfn_NvVFX_SetImage(c->handle, NVVFX_OUTPUT_IMAGE, &c->out_img);
    if (ret != 0) {
        fprintf(stderr, "VSR: SetImage(output %dx%d) failed (%d)\n",
                out_w, out_h, ret);
        return false;
    }
    c->out_pixels = c->out_img.pixels;
    c->out_pitch = (int)c->out_img.pitch;
    c->out_w = out_w;
    c->out_h = out_h;
    fprintf(stderr, "VSR: output %dx%d -> %dx%d (engine kept)\n",
            (int)c->in_w, (int)c->in_h, out_w, out_h);
    return true;
}

bool vsr_set_quality(struct vsr_context *c, int quality) {
    if (!c->handle || !c->ready) return false;
    int ret = pfn_NvVFX_SetU32(c->handle, "QualityLevel", (unsigned)quality);
    if (ret != 0) {
        fprintf(stderr, "VSR: SetU32(QualityLevel=%d) failed (%d)\n",
                quality, ret);
        return false;
    }
    c->quality = quality;
    fprintf(stderr, "VSR: quality -> %d (engine kept)\n", quality);
    return true;
}

// ── vsr_destroy ──────────────────────────────────────────────────────────
// 销毁前等待在途工作完成：Run 为异步提交，在途推理若仍在 GPU 上执行，
// Dealloc/DestroyEffect 释放其目标缓冲会导致 CUDA 非法访问（ILLEGAL_ADDRESS
// / GPU hang）。
//
// 同步范围 = 引擎流（c->stream，阻塞流，见 vsr_init；推理完成即流空闲）。
// 不能做设备级同步（cuCtxSynchronize）：它会把 VO map 的 stream 0（GUI
// 渲染线程的异步拷贝）也卷进来等——渲染循环随 mpv 核心阻塞而停止后，
// stream 0 的拷贝永远不完成 → ctx sync 死锁（Xid 109 场景 core 实测：
// destroy 卡在 cuCtxSynchronize，stream 0 NOT_READY）。调用方须已
// cuCtxPushCurrent。

void vsr_destroy(struct vsr_context *c) {
    if (c->stream)
        cuStreamSynchronize(c->stream);
    cuStreamSynchronize(0);
    if (c->handle) {
        pfn_NvVFX_DestroyEffect(c->handle);
        c->handle = NULL;
    }
    if (c->out_img.pixels) {
        pfn_NvCVImage_Dealloc(&c->out_img);
        memset(&c->out_img, 0, sizeof(c->out_img));
    }
    if (c->in_img.pixels) {
        pfn_NvCVImage_Dealloc(&c->in_img);
        memset(&c->in_img, 0, sizeof(c->in_img));
    }
    if (c->tmp_img.pixels) {
        pfn_NvCVImage_Dealloc(&c->tmp_img);
        memset(&c->tmp_img, 0, sizeof(c->tmp_img));
    }
    if (c->own_stream && c->stream) {
        cuStreamDestroy(c->stream);
        c->stream = NULL;
        c->own_stream = false;
    }
    c->ready = false;
    c->in_w = c->in_h = c->out_w = c->out_h = 0;
}
