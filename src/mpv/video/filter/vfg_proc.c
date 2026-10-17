// src/mpv/video/filter/vfg_proc.c
// VFG（VideoFrameGeneration）引擎封装实现（接口见 vfg_proc.h）。
// 结构镜像 vsr_proc.c：per-context NvCVImage 缓冲、按需分配（尺寸变化先
// Dealloc 再 Alloc，杜绝"旧缓冲未释放即被覆盖"）、销毁前同步在途工作。
//
// 日志：本层没有 filter 上下文（与 vsr_proc.c 相同），用 fprintf(stderr)
// 加 "VFG:" 前缀；vf_vfg.c 侧一律走 mp_msg(f->log, ...)。
#include "vfg_proc.h"
#include "yuv_to_rgba.h"   // VFX_BITS_10 / vfx_* 格式映射（单一事实源）

#include <cuda.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VFG_INPUT0_IMAGE  "SrcImage0"
#define VFG_INPUT1_IMAGE  "SrcImage1"
#define VFG_OUTPUT_IMAGE  "DstImage0"
#define VFG_CUDA_STREAM   "CudaStream"

// ── 图像缓冲按需分配（镜像 vsr_proc.c ensure_img）──────────────────────

static bool ensure_img(NvCVImage *img, int w, int h, int bits, const char *tag)
{
    int pf = (bits == VFX_BITS_10) ? NVCV_RGB10A2 : NVCV_RGBA;
    int ct = (bits == VFX_BITS_10) ? NVCV_P32     : NVCV_U8;
    if (img->pixels && img->width == (unsigned)w && img->height == (unsigned)h &&
        img->pixelFormat == pf && img->componentType == ct)
        return true;
    if (img->pixels) {
        pfn_NvCVImage_Dealloc(img);
        memset(img, 0, sizeof(*img));
    }
    int ar = pfn_NvCVImage_Alloc(img, (unsigned)w, (unsigned)h,
                                 pf, ct, NVCV_CHUNKY, NVCV_GPU, 32);
    if (ar != 0) {
        fprintf(stderr, "VFG: Alloc(%s %dx%d %d-bit) failed (%d)\n",
                tag, w, h, bits, ar);
        memset(img, 0, sizeof(*img));
        return false;
    }
    return true;
}

// ── vfg_init ─────────────────────────────────────────────────────────────
// 参数集合与顺序照 vfx13_probe.c:151-179（本机实测通过：1280x720 单峰
// 落在中点 = 真合成中间帧）。探针在 Load 前设置全部参数与图像。

bool vfg_init(struct vfg_context *c, int in_w, int in_h, int mode,
              int multiplier, int bits, CUstream stream)
{
    if (!vfx_load_libraries())
        return false;
    if (multiplier < VFG_MULT_MIN || multiplier > VFG_MULT_MAX) {
        fprintf(stderr, "VFG: FrameMultiplier %d out of range (%d..%d)\n",
                multiplier, VFG_MULT_MIN, VFG_MULT_MAX);
        return false;
    }

    memset(c, 0, sizeof(*c));
    c->in_w = in_w;
    c->in_h = in_h;
    c->mode = mode;
    c->multiplier = multiplier;
    c->bits = (bits == VFX_BITS_10) ? VFX_BITS_10 : VFX_BITS_8;

    int ret = pfn_NvVFX_CreateEffect("VideoFrameGeneration", &c->handle);
    if (ret != 0) {
        fprintf(stderr, "VFG: CreateEffect(VideoFrameGeneration) failed (%d)\n",
                ret);
        return false;
    }

    // InputWidth/InputHeight 必须在 Load 前设定（官方包装标 requires_reload，
    // 且在 load() 内于 Load 之前 SetU32）。
    pfn_NvVFX_SetU32(c->handle, "InputWidth", (unsigned int)in_w);
    pfn_NvVFX_SetU32(c->handle, "InputHeight", (unsigned int)in_h);
    pfn_NvVFX_SetU32(c->handle, "Mode", (unsigned int)mode);
    // FrameMultiplier/FrameIndex 是**每次 Run**的参数（官方包装的 run()
    // 每次重新 SetU32，且未标 requires_reload）；在 Load 前也设一次是为了
    // 与探针完全一致的初始状态。
    pfn_NvVFX_SetU32(c->handle, "FrameMultiplier", (unsigned int)multiplier);
    pfn_NvVFX_SetU32(c->handle, "FrameIndex", 1);
    // 场景切换检测交给引擎（单一事实源）：vf_vfg 不再实现第二套 MAE。
    pfn_NvVFX_SetU32(c->handle, "AutomaticShotChangeDetectionEnabled", 1);

    // ── 两张输入 + 一张输出：RGBA U8 / RGB10A2 chunky GPU ─────────────
    // 三张图编码必须一致（库内文档：RGB8/RGB10A2 mixing is unsupported）。
    for (int i = 0; i < 2; i++) {
        if (!ensure_img(&c->in_img[i], in_w, in_h, c->bits, i ? "src1" : "src0")) {
            vfg_destroy(c);
            return false;
        }
        c->in_pixels[i] = c->in_img[i].pixels;
        c->in_pitch[i]  = (int)c->in_img[i].pitch;
    }
    // VFG 输出与输入同尺寸（探针：ow/oh = W/H）
    if (!ensure_img(&c->out_img, in_w, in_h, c->bits, "dst")) {
        vfg_destroy(c);
        return false;
    }
    c->out_pixels = c->out_img.pixels;
    c->out_pitch  = (int)c->out_img.pitch;

    ret = pfn_NvVFX_SetImage(c->handle, VFG_INPUT0_IMAGE, &c->in_img[0]);
    if (ret != 0) {
        fprintf(stderr, "VFG: SetImage(SrcImage0) failed (%d)\n", ret);
        vfg_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, VFG_INPUT1_IMAGE, &c->in_img[1]);
    if (ret != 0) {
        fprintf(stderr, "VFG: SetImage(SrcImage1) failed (%d)\n", ret);
        vfg_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, VFG_OUTPUT_IMAGE, &c->out_img);
    if (ret != 0) {
        fprintf(stderr, "VFG: SetImage(DstImage0) failed (%d)\n", ret);
        vfg_destroy(c);
        return false;
    }

    // ── CUDA stream ───────────────────────────────────────────────────────
    if (stream) {
        c->stream = stream;
        c->own_stream = false;
    } else {
        cuStreamCreate(&c->stream, CU_STREAM_NON_BLOCKING);
        c->own_stream = true;
    }
    pfn_NvVFX_SetCudaStream(c->handle, VFG_CUDA_STREAM, c->stream);

    // ── Load（模型初始化；1.3 的 VFG 库 218MB，Load 成本远高于单帧）────
    ret = pfn_NvVFX_Load(c->handle);
    if (ret != 0) {
        fprintf(stderr, "VFG: Load failed (%d)\n", ret);
        vfg_destroy(c);
        return false;
    }

    c->ready = true;
    fprintf(stderr, "VFG: init ok (%dx%d mode=%d k=%d)\n",
            in_w, in_h, mode, multiplier);
    return true;
}

// ── vfg_reconfig ─────────────────────────────────────────────────────────
// 输入尺寸或 Mode 变更：销毁 + 重建 + 重新 Load（理由见 vfg_proc.h）。

bool vfg_reconfig(struct vfg_context *c, int in_w, int in_h, int mode,
                  int multiplier, int bits, CUstream stream)
{
    vfg_destroy(c);
    return vfg_init(c, in_w, in_h, mode, multiplier, bits, stream);
}

// ── vfg_set_multiplier ───────────────────────────────────────────────────

bool vfg_set_multiplier(struct vfg_context *c, int multiplier)
{
    if (!c->handle || !c->ready)
        return false;
    if (multiplier < VFG_MULT_MIN || multiplier > VFG_MULT_MAX) {
        fprintf(stderr, "VFG: FrameMultiplier %d out of range (%d..%d)\n",
                multiplier, VFG_MULT_MIN, VFG_MULT_MAX);
        return false;
    }
    c->multiplier = multiplier;
    return true;
}

// ── vfg_process ──────────────────────────────────────────────────────────
// 每次 Run 生成 1 张插值帧（FrameIndex 由调用方按 i=1..multiplier-1 给出；
// 官方包装的 run() 同样是"每次 Run 前 SetU32(FrameMultiplier)+
// SetU32(FrameIndex)"，无需重新 Load）。
// 输出缓冲直接取 c->out_img.pixels：缓冲由本层 Alloc 并经 SetImage 交给
// 引擎，地址稳定（探针就是这么回读的，不依赖 GetImage）。

bool vfg_process(struct vfg_context *c, int frame_index,
                 void **output_ptr, int *out_pitch)
{
    if (!c->ready)
        return false;
    if (frame_index < 1 || frame_index >= c->multiplier) {
        fprintf(stderr, "VFG: FrameIndex %d out of range (1..%d)\n",
                frame_index, c->multiplier - 1);
        return false;
    }

    pfn_NvVFX_SetU32(c->handle, "FrameMultiplier", (unsigned int)c->multiplier);
    pfn_NvVFX_SetU32(c->handle, "FrameIndex", (unsigned int)frame_index);

    int ret = pfn_NvVFX_Run(c->handle, 0);
    if (ret != 0) {
        fprintf(stderr, "VFG: Run(FrameIndex=%d) failed (%d)\n", frame_index, ret);
        return false;
    }

    if (output_ptr) *output_ptr = c->out_img.pixels;
    if (out_pitch)  *out_pitch  = (int)c->out_img.pitch;
    return true;
}

// ── vfg_destroy ──────────────────────────────────────────────────────────
// 与 vsr_destroy 同款：先同步在途工作（Run 异步提交，引擎内部可能仍在写
// 输出缓冲——此时 DestroyEffect/Dealloc 释放目标缓冲会触发 CUDA 非法访问）。
// 同步范围 = 自己的流（不做事关设备的 cuCtxSynchronize：那会把 VO 的
// stream 0 拷贝卷进来，渲染循环随核心阻塞后死锁——Xid 109 教训）。
// 调用方须已 cuCtxPushCurrent。

void vfg_destroy(struct vfg_context *c)
{
    if (c->stream)
        cuStreamSynchronize(c->stream);
    cuStreamSynchronize(0);
    if (c->handle) {
        pfn_NvVFX_DestroyEffect(c->handle);
        c->handle = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (c->in_img[i].pixels) {
            pfn_NvCVImage_Dealloc(&c->in_img[i]);
            memset(&c->in_img[i], 0, sizeof(c->in_img[i]));
        }
        c->in_pixels[i] = NULL;
        c->in_pitch[i] = 0;
    }
    if (c->out_img.pixels) {
        pfn_NvCVImage_Dealloc(&c->out_img);
        memset(&c->out_img, 0, sizeof(c->out_img));
    }
    c->out_pixels = NULL;
    c->out_pitch = 0;
    if (c->own_stream && c->stream) {
        cuStreamDestroy(c->stream);
        c->stream = NULL;
        c->own_stream = false;
    }
    c->ready = false;
    c->in_w = c->in_h = 0;
}
