// src/mpv/video/filter/vf_truehdr.c
// VFX TrueHDR（SDR → HDR10 上变换）——VFX SDK 的 AI 上变换 effect，
// 接在 `vf_vsr` **之后**（方案见 docs/vfx-truehdr-investigation.md §8）。
//
// 契约（全部有实测依据，卷号指该文档）：
//  输入：CUDA 帧，chunky RGB——RGBA8（8-bit 通路）或 X2BGR10（10-bit 容器的
//    **SDR** 源，先经 NvCVImage_Transfer 落到 RGBA8）；YUV（NV12/P010，VSR
//    直通时上游就是原始解码帧）经共用的 yuv_to_rgba 内核转 RGBA8。TrueHDR
//    要求 SDR 编码（Rec.709/D65/≈gamma2.2、全范围）。
//  输出：CUDA 帧 + hw_subfmt=X2BGR10（VFX RGB10A2 = mpv IMGFMT_X2BGR10），
//    色彩参数置 **BT.2020 / PQ**——下游全是既有 10-bit 通路（vfx_bits_from_imgfmt
//    =10、VFG 走 D2D、VO 按 HDR 处理）。
//  HDR 内容直通：按**帧的 transfer**（PQ/HLG）自守卫，不依赖客户端开关
//   （单一事实源；客户端开关只负责"要不要做上变换"）。
//  DebangingOff 恒 0（抗色带常开，成本恒定、收益条件——AGENTS 工作铁律 6）。
//
// 引擎生命周期照 vf_vsr.c：lazy init、换 CUDA context 时**在旧 context 内**
// 销毁引擎/转换器/流/缓冲（见 AGENTS「运行中切 hwdec」的连环坑）。
// **参数变化不重建引擎**：Load 之后 SetU32 生效且与重建逐位一致（实测见
// engine_params_apply 的注释）——UI 滑块拖动因此可以只发 vf-command。

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/csputils.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "common/common.h"

#include <cuda.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include "vfx_loader.h"
#include "cuda_shared.h"
#include "yuv_to_rgba.h"

static double truehdr_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ── Options ────────────────────────────────────────────────────────────────
// 链串里恒 `enable=0`（默认关）：实际状态由客户端按 HDR 策略经 vf-command
// 下发（opt-in env `VSR_UPHDR=on`），见 docs/vfx-truehdr-investigation.md §8.2/8.4。

struct vf_truehdr_opts {
    bool enable;        // false = 直通（默认）；true = 上变换（HDR 内容仍自守卫直通）
    int luminance;      // 目标显示峰值 nit（SDK 参数，400–2000；客户端下面板物理峰值）
    int saturation;     // 100 = SDK 默认
    int contrast;       // 100 = SDK 默认
    int middle_gray;    // 50  = SDK 默认（范围 10–100，见库内参数文档串）
    bool debanding_off; // false = 抗色带**开**（默认/推荐；仅诊断用 true）
};

#define OPT_BASE_STRUCT struct vf_truehdr_opts

static const struct m_option vf_opts_fields[] = {
    {"enable",        OPT_BOOL(enable)},
    {"luminance",     OPT_INT(luminance), M_RANGE(400, 2000)},
    {"saturation",    OPT_INT(saturation), M_RANGE(0, 200)},
    {"contrast",      OPT_INT(contrast), M_RANGE(0, 200)},
    {"middle-gray",   OPT_INT(middle_gray), M_RANGE(10, 100)},
    {"debanding-off", OPT_BOOL(debanding_off)},
    {0}
};

static const struct vf_truehdr_opts vf_truehdr_opts_def = {
    .enable        = false,
    .luminance     = 650,
    .saturation    = 100,
    .contrast      = 100,
    .middle_gray   = 50,
    .debanding_off = false,
};

// ── Filter private state ───────────────────────────────────────────────────

struct priv {
    struct vf_truehdr_opts *opts;

    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    // ── TrueHDR 引擎（NvVFX effect + 两张 GPU 图）──────────────────────
    void      *effect;
    NvCVImage  in_img;        // RGBA8（引擎输入，我方拥有）
    NvCVImage  in10_img;      // RGB10A2（10-bit SDR 源的中转，lazy）
    NvCVImage  out_img;       // RGB10A2（引擎输出，我方拥有）
    bool       imgs_alloc;    // in_img/out_img 已分配
    bool       in10_alloc;
    int        eng_w, eng_h;
    bool       loaded;
    // 引擎参数快照（变化即重建；不假设 SetU32 在 Load 后生效）
    int        e_luminance, e_saturation, e_contrast, e_middle_gray, e_debanding_off;
    bool       failed;        // 创建/Load 失败 → 永久直通（不每帧重试）
    bool       failed_warned;

    // ── YUV → RGBA8（共用内核；VSR 直通/关闭时上游是原始解码帧）────────
    struct yuv_to_rgba yuv_conv;
    bool                yuv_conv_ready;
    int                 yuv_conv_bd;
    enum yuv_matrix     yuv_conv_matrix;
    enum yuv_range      yuv_conv_range;

    // ── 输出缓冲（拥有权逐帧移交）──────────────────────────────────────
    AVBufferRef *av_hw_device;
    AVBufferRef *av_hw_frames;
    CUcontext    av_hw_frames_ctx;
    CUdeviceptr  out_buf;
    size_t       out_buf_size;
    int          out_pitch;

    int  video_w, video_h;

    bool sw_warned;      // 软解帧（链上无 hwup）直通只警告一次
    bool hdr_warned;     // HDR 内容直通日志只打一次
    bool fmt_warned;     // 不支持的输入格式警告只一次
    bool fmt_logged;     // 首次上变换的格式日志
    int  frame_count;

    // truehdr-status（每 30 帧）
    double status_t0;
    int    status_frames;
    double status_cost_ms;
    int    status_emit;
    double last_peak_nit, last_med_nit;
};

// ── CUDA context 绑定（vf_vsr.c ensure_cuda 的同构裁剪版）─────────────────

static bool ensure_cuda(struct mp_filter *f, struct priv *p,
                        struct mp_image *mpi)
{
    CUcontext borrow = mp_cuda_ctx_from_mpi(mpi);   // NULL = SW frame

    if (p->cuda_ref) {
        if (p->cuda_ref->ctx == borrow)
            return true;
    }

    if (cuInit(0) != CUDA_SUCCESS) {
        MP_ERR(f, "truehdr: cuInit failed\n");
        return false;
    }

    // ⚠ 所有 CUDA 资源都绑在创建它的 context 上：换 context（切软/硬解、
    // 上游换上下文）后复用旧句柄 → cuLaunchKernel/CUDA API 返回 400 →
    // 滤镜被 mpv 禁用（vf_vsr.c 同款实测）。必须在**旧 context 内**全部作废。
    if (p->cuda_ref) {
        CUcontext old = p->cuda_ref->ctx;
        if (old) cuCtxPushCurrent(old);
        if (p->loaded || p->effect) {
            if (p->effect) pfn_NvVFX_DestroyEffect(p->effect);
            p->effect = NULL;
            p->loaded = false;
        }
        if (p->imgs_alloc) {
            pfn_NvCVImage_Dealloc(&p->in_img);
            pfn_NvCVImage_Dealloc(&p->out_img);
            p->imgs_alloc = false;
        }
        if (p->in10_alloc) {
            pfn_NvCVImage_Dealloc(&p->in10_img);
            p->in10_alloc = false;
        }
        if (p->yuv_conv_ready) {
            yuv_to_rgba_destroy(&p->yuv_conv);
            p->yuv_conv_ready = false;
        }
        if (p->out_buf) {
            cuMemFree(p->out_buf);
            p->out_buf = 0;
            p->out_buf_size = 0;
        }
        if (p->cuda_init_done && p->cuda_stream) {
            cuStreamDestroy(p->cuda_stream);
            p->cuda_stream = NULL;
            p->cuda_init_done = false;
        }
        if (old) cuCtxPopCurrent(NULL);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        p->failed = false;       // 换 context 是一次重建机会（失败可能与此有关）
        p->fmt_logged = false;
        MP_VERBOSE(f, "truehdr: CUDA context changed — engine reset\n");
    }

    struct mp_cuda_ctx_ref *shared = mp_cuda_ctx_ref_from_mpi(mpi);
    if (shared) {
        mp_cuda_ctx_ref_acquire(shared);
        p->cuda_ref = shared;
    } else if (borrow) {
        p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
        p->cuda_ref->magic = MP_CUDA_CTX_REF_MAGIC;
        p->cuda_ref->ctx = borrow;
        p->cuda_ref->owned = false;
        p->cuda_ref->ctx_holder = av_buffer_ref(mpi->hwctx);
        if (!p->cuda_ref->ctx_holder) {
            MP_ERR(f, "truehdr: failed to hold hwdec frame ref\n");
            free(p->cuda_ref);
            p->cuda_ref = NULL;
            return false;
        }
        atomic_init(&p->cuda_ref->refs, 1);
    } else {
        MP_ERR(f, "truehdr: no CUDA context from input (software frame past passthrough)\n");
        return false;
    }

    cuCtxPushCurrent(p->cuda_ref->ctx);
    // 必须是**阻塞流**（VFX 1.3 内部有固定挂在 legacy 默认流上的环节，
    // 非阻塞流与 legacy 流没有隐含顺序 → 读到未落地的输入；vf_vsr.c 同款）。
    CUresult cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_DEFAULT);
    cuCtxPopCurrent(NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "truehdr: cuStreamCreate failed (%d)\n", cr);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        return false;
    }

    p->cuda_init_done = true;
    return true;
}

// ── YUV→RGBA8 转换器生命周期（共用 yuv_to_rgba 内核）──────────────────────

static bool ensure_yuv_converter(struct mp_filter *f, struct priv *p,
                                 int bit_depth, enum yuv_matrix matrix,
                                 enum yuv_range range)
{
    if (p->yuv_conv_ready && p->yuv_conv_bd == bit_depth &&
        p->yuv_conv_matrix == matrix && p->yuv_conv_range == range)
        return true;

    if (p->yuv_conv_ready)
        yuv_to_rgba_destroy(&p->yuv_conv);

    // TrueHDR 输入必须是 8-bit SDR（RGBA8）→ out_bits 恒 VFX_BITS_8。
    if (!yuv_to_rgba_init(&p->yuv_conv, bit_depth, matrix, range, VFX_BITS_8)) {
        MP_ERR(f, "truehdr: yuv_to_rgba_init failed (bd=%d)\n", bit_depth);
        p->yuv_conv_ready = false;
        return false;
    }
    p->yuv_conv_bd = bit_depth;
    p->yuv_conv_matrix = matrix;
    p->yuv_conv_range = range;
    p->yuv_conv_ready = true;
    return true;
}

// ── 引擎（NvVFX effect "TrueHDR"）生命周期 ────────────────────────────────
// 参数变化**走 SetU32 热更新**（实测 Load 后生效，与重建逐位一致；见下方
// engine_params_apply），只有尺寸变化 / DebandingOff / SetU32 失败才重建。

static void engine_destroy(struct priv *p)
{
    if (p->effect)
        pfn_NvVFX_DestroyEffect(p->effect);
    p->effect = NULL;
    p->loaded = false;
    if (p->imgs_alloc) {
        pfn_NvCVImage_Dealloc(&p->in_img);
        pfn_NvCVImage_Dealloc(&p->out_img);
        p->imgs_alloc = false;
    }
    // 10-bit 中转图也必须作废：它按**当时**的帧尺寸分配，尺寸变化（换片）后
    // 复用它会让 D2D 拷贝越界（引擎按新尺寸重建，中转图还是旧的）。
    if (p->in10_alloc) {
        pfn_NvCVImage_Dealloc(&p->in10_img);
        p->in10_alloc = false;
    }
}

static bool engine_create(struct mp_filter *f, struct priv *p, int w, int h)
{
    struct vf_truehdr_opts *o = p->opts;

    engine_destroy(p);

    int ra = pfn_NvCVImage_Alloc(&p->in_img, w, h, NVCV_RGBA, NVCV_U8,
                                 NVCV_CHUNKY, NVCV_GPU, 32);
    int rb = pfn_NvCVImage_Alloc(&p->out_img, w, h, NVCV_RGB10A2, NVCV_P32,
                                 NVCV_CHUNKY, NVCV_GPU, 32);
    if (ra != 0 || rb != 0) {
        MP_ERR(f, "truehdr: NvCVImage_Alloc failed (%d/%d) %dx%d\n", ra, rb, w, h);
        if (ra == 0) pfn_NvCVImage_Dealloc(&p->in_img);
        if (rb == 0) pfn_NvCVImage_Dealloc(&p->out_img);
        return false;
    }
    p->imgs_alloc = true;

    int cr = pfn_NvVFX_CreateEffect("TrueHDR", &p->effect);
    if (cr != 0 || !p->effect) {
        MP_ERR(f, "truehdr: NvVFX_CreateEffect(TrueHDR) failed (%d)\n", cr);
        engine_destroy(p);
        return false;
    }

    pfn_NvVFX_SetU32(p->effect, "Contrast",     (unsigned)o->contrast);
    pfn_NvVFX_SetU32(p->effect, "Saturation",   (unsigned)o->saturation);
    pfn_NvVFX_SetU32(p->effect, "MiddleGray",   (unsigned)o->middle_gray);
    pfn_NvVFX_SetU32(p->effect, "Luminance",    (unsigned)o->luminance);
    // 抗色带**常开**（0 = 开）：成本恒定（实测 1.6–4.3×，与内容无关）、
    // 收益条件（只有带色带的镜头受益）——不假设画面里"没有色带"（铁律 6）。
    pfn_NvVFX_SetU32(p->effect, "DebandingOff", (unsigned)o->debanding_off);
    pfn_NvVFX_SetImage(p->effect, "SrcImage0", &p->in_img);
    pfn_NvVFX_SetImage(p->effect, "DstImage0", &p->out_img);
    pfn_NvVFX_SetCudaStream(p->effect, "CudaStream", p->cuda_stream);

    double t0 = truehdr_now_ms();
    int lr = pfn_NvVFX_Load(p->effect);
    if (lr != 0) {
        MP_ERR(f, "truehdr: NvVFX_Load failed (%d) — 上变换不可用，直通\n", lr);
        engine_destroy(p);
        return false;
    }
    MP_INFO(f, "truehdr: engine ready %dx%d (Load %.0f ms) luminance=%d "
            "contrast=%d saturation=%d middle-gray=%d debanding=%s\n",
            w, h, truehdr_now_ms() - t0, o->luminance, o->contrast, o->saturation,
            o->middle_gray, o->debanding_off ? "OFF(diagnostic)" : "on");

    p->loaded = true;
    p->eng_w = w;
    p->eng_h = h;
    p->e_luminance = o->luminance;
    p->e_saturation = o->saturation;
    p->e_contrast = o->contrast;
    p->e_middle_gray = o->middle_gray;
    p->e_debanding_off = o->debanding_off;
    return true;
}

/// 参数**轻量更新**：Load 之后 `NvVFX_SetU32` 对 TrueHDR 生效，且与"重新 Load
/// 同参数"的输出**逐位一致**。实测（`build/scratch/truehdr_setu32.c`，1920x1080
/// 合成图、同一引擎只 SetU32 后重跑）：
///   Saturation 100→200 : 采样色度 0.1627 → 0.2377
///   MiddleGray 50→80   : 采样均值 24.1 → 38.7 nit
///   Luminance 400→1000 : 灰阶带均值 74.6 → 88.0 nit
///   对照组（destroy+create+Load 同参数）= 0.2537 / 38.7 nit —— **与 SetU32 路径相同**
/// 所以 UI 拖动参数（节流后每秒数次）不需要重建引擎：SetU32 即可，零卡顿。
/// ⚠ `DebandingOff` **没测**（它改的是 pass 图，"跳过 DL-Debander"很可能是 Load 期
/// 决定）→ 该参数变化一律返回 false，走重建路径（不假设）。
static bool engine_params_apply(struct mp_filter *f, struct priv *p)
{
    const struct vf_truehdr_opts *o = p->opts;
    if (p->e_debanding_off != o->debanding_off) {
        MP_INFO(f, "truehdr: debanding %d -> %d（重建引擎；该参数是 Load 期行为）\n",
                p->e_debanding_off, o->debanding_off);
        return false;
    }
    if (p->e_luminance == o->luminance && p->e_saturation == o->saturation &&
        p->e_contrast == o->contrast && p->e_middle_gray == o->middle_gray)
        return true;   // 无变化

    bool ok = true;
    if (p->e_contrast != o->contrast)
        ok &= pfn_NvVFX_SetU32(p->effect, "Contrast", (unsigned)o->contrast) == 0;
    if (p->e_saturation != o->saturation)
        ok &= pfn_NvVFX_SetU32(p->effect, "Saturation", (unsigned)o->saturation) == 0;
    if (p->e_middle_gray != o->middle_gray)
        ok &= pfn_NvVFX_SetU32(p->effect, "MiddleGray", (unsigned)o->middle_gray) == 0;
    if (p->e_luminance != o->luminance)
        ok &= pfn_NvVFX_SetU32(p->effect, "Luminance", (unsigned)o->luminance) == 0;
    if (!ok) {
        MP_WARN(f, "truehdr: SetU32 失败 → 重建引擎兜底\n");
        return false;
    }
    MP_VERBOSE(f, "truehdr: params %d/%d/%d/%d -> %d/%d/%d/%d (SetU32, 无重建)\n",
               p->e_contrast, p->e_saturation, p->e_middle_gray, p->e_luminance,
               o->contrast, o->saturation, o->middle_gray, o->luminance);
    p->e_contrast = o->contrast;
    p->e_saturation = o->saturation;
    p->e_middle_gray = o->middle_gray;
    p->e_luminance = o->luminance;
    return true;
}

// ── 输出缓冲 / 帧构造（vf_vsr.c 输出段同构）───────────────────────────────

static bool ensure_out_buf(struct mp_filter *f, struct priv *p, int pitch, int h)
{
    size_t needed = (size_t)pitch * h;
    if (p->out_buf && p->out_buf_size >= needed) {
        p->out_pitch = pitch;
        return true;
    }
    if (p->out_buf) {
        cuMemFree(p->out_buf);
        p->out_buf = 0;
        p->out_buf_size = 0;
    }
    if (cuMemAlloc(&p->out_buf, needed) != CUDA_SUCCESS) {
        MP_ERR(f, "truehdr: cuMemAlloc(out) failed (%zu bytes)\n", needed);
        return false;
    }
    p->out_buf_size = needed;
    p->out_pitch = pitch;
    return true;
}

static void mp_image_dtor(void *ptr)
{
    struct mp_image *mpi = ptr;
    for (int p = 0; p < MP_MAX_PLANES; p++)
        av_buffer_unref(&mpi->bufs[p]);
    av_buffer_unref(&mpi->hwctx);
    av_buffer_unref(&mpi->icc_profile);
    av_buffer_unref(&mpi->a53_cc);
    av_buffer_unref(&mpi->dovi);
    av_buffer_unref(&mpi->film_grain);
    for (int n = 0; n < mpi->num_ff_side_data; n++)
        av_buffer_unref(&mpi->ff_side_data[n].buf);
}

static void free_cuda_buf(void *opaque, uint8_t *data)
{
    struct mp_cuda_ctx_ref *r = opaque;
    if (r && r->ctx) cuCtxPushCurrent(r->ctx);
    // 不做流同步：帧只在消费端替换后释放，本滤镜每帧的流同步已把拷贝排干；
    // 对共享 context 的 stream 0 同步会等渲染线程（vf_vsr.c 同款理由）。
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        mp_cuda_ctx_ref_release(r);
}

// 调用方须已 cuCtxPushCurrent；p->out_buf 的拥有权移交给返回帧。
static struct mp_image *make_output(struct mp_filter *f, struct priv *p,
                                    const struct mp_image *src, int w, int h)
{
    if (!mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx, AV_PIX_FMT_RGBA,
                               w, h, &p->av_hw_device, &p->av_hw_frames,
                               &p->av_hw_frames_ctx))
        return NULL;

    mp_cuda_ctx_ref_acquire(p->cuda_ref);
    struct mp_image *out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t *)p->out_buf, (int)p->out_buf_size,
                                    free_cuda_buf, p->cuda_ref, 0);
    if (!out->bufs[0]) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        talloc_free(out);
        return NULL;
    }
    mp_image_setfmt(out, IMGFMT_CUDA);
    // 输出 = VFX RGB10A2 = IMGFMT_X2BGR10；sw_format 恒 RGBA（FFmpeg CUDA
    // hwframes 不支持 x2bgr10，10-bit 由 hw_subfmt 承载——vf_vsr.c 同款）。
    mp_image_sethwfmt(out, IMGFMT_CUDA, IMGFMT_X2BGR10);
    mp_image_params_guess_csp(&out->params);
    // 色彩元数据**对齐 mpv 自己的 NVIDIA True HDR 参考实现**
    //（third_party/mpv/video/filter/vf_d3d11vpp.c:410）：输出标为 HDR10
    //（pl_color_space_hdr10 = BT.2020/PQ/light=HDR10）+ 显式 content peak。
    // mpv 在 D3D 路径上只能"猜" max_luma=1000 并警告；我们这里**知道**
    // 目标峰值（SDK 的 Luminance 参数）→ 如实标注（口径与 video-params/
    // max-luma 同为绝对 nit，供下游 tone mapping 判定内容峰值）。
    out->params.color = pl_color_space_hdr10;
    out->params.color.hdr.max_luma = p->e_luminance;
    out->params.p_w = src->params.p_w;
    out->params.p_h = src->params.p_h;
    out->w = w;
    out->h = h;
    out->params.w = w;
    out->params.h = h;
    out->planes[0] = (uint8_t *)p->out_buf;
    out->stride[0] = p->out_pitch;
    out->num_planes = 1;
    out->hwctx = av_buffer_ref(p->av_hw_frames);
    out->pts         = src->pts;
    out->dts         = src->dts;
    out->nominal_fps = src->nominal_fps;
    p->out_buf = 0;          // 拥有权移交
    p->out_buf_size = 0;
    return out;
}

// ── 输出采样（status 行的数值证据：PQ 码值 → nit）────────────────────────

static double pq_code_to_nits(double c)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
                 c3 = 2392.0 / 4096.0 * 32.0;
    double p = pow(c, 1.0 / m2);
    double num = p - c1;
    if (num < 0) num = 0;
    double den = c2 - c3 * p;
    if (den <= 0) return 0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

/// 从引擎输出图里采样一行（读回 32 个点）→ nit 中位数/峰值。调用方须已同步流。
static void sample_output_nits(struct priv *p, double *med, double *peak)
{
    const int n = 32;
    unsigned int words[32];
    double nits[32];
    int y = p->eng_h / 2, w = p->eng_w;
    for (int i = 0; i < n; i++) {
        int x = (int)((double)i / (n - 1) * (w - 1));
        words[i] = 0;
        if (cuMemcpyDtoH(&words[i], (CUdeviceptr)p->out_img.pixels +
                         (size_t)y * p->out_img.pitch + (size_t)x * 4, 4)
            != CUDA_SUCCESS)
            words[i] = 0;
        unsigned int wd = words[i];
        double r = (wd & 0x3FF) / 1023.0, g = ((wd >> 10) & 0x3FF) / 1023.0,
               b = ((wd >> 20) & 0x3FF) / 1023.0;
        nits[i] = (pq_code_to_nits(r) + pq_code_to_nits(g) + pq_code_to_nits(b)) / 3.0;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (nits[j] < nits[i]) { double t = nits[i]; nits[i] = nits[j]; nits[j] = t; }
    *med = nits[n / 2];
    *peak = nits[n - 1];
}

// ── status 行（每 30 帧；OSD/日志的单一来源，与 vsr-status 同频道）────────

static void emit_status(struct mp_filter *f, struct priv *p, const char *mode)
{
    if (++p->status_emit < 30)
        return;
    p->status_emit = 0;
    double el = p->status_t0 > 0 ? (truehdr_now_ms() - p->status_t0) / 1000.0 : 0;
    if (strcmp(mode, "active") == 0 && el > 0) {
        mp_msg(f->log, MSGL_STATUS,
               "truehdr-status: mode=active cost=%.1fms fps=%.0f luminance=%d "
               "peak=%.0fnit med=%.0fnit\n",
               p->status_cost_ms / (p->status_frames ? p->status_frames : 1),
               p->status_frames / el, p->e_luminance, p->last_peak_nit,
               p->last_med_nit);
    } else {
        mp_msg(f->log, MSGL_STATUS, "truehdr-status: mode=passthrough\n");
    }
    p->status_t0 = truehdr_now_ms();
    p->status_frames = 0;
    p->status_cost_ms = 0;
}

// ── f_process ──────────────────────────────────────────────────────────────

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);

    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO) {
        MP_ERR(f, "truehdr: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *mpi = frame.data;
    p->frame_count++;

    // ── 直通判定（逐帧，不缓存）─────────────────────────────────────────
    //  ① 关闭（默认）② 引擎不可用 ③ HDR 内容（transfer PQ/HLG——自守卫，
    //  不依赖客户端开关）④ 非 CUDA 帧（软解且链上无 hwup）。
    const bool is_hw = (mpi->imgfmt == IMGFMT_CUDA);
    // HDR 内容判据对齐 mpv 参考实现（vf_d3d11vpp.c:151 的 want_nvidia_true_hdr）：
    // `!pl_color_space_is_hdr(...)` 才做上变换（libplacebo 口径：有效信号峰值
    // 高于 SDR 参考白即 HDR——覆盖 PQ/HLG 及带 hdr 元数据的场合）。
    const bool content_hdr = pl_color_space_is_hdr(&mpi->params.color);

    if (!is_hw) {
        if (!p->sw_warned) {
            MP_WARN(f, "truehdr: software frames — passthrough (prepend @hwup)\n");
            p->sw_warned = true;
        }
        emit_status(f, p, "passthrough");
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (content_hdr) {
        if (!p->hdr_warned) {
            MP_INFO(f, "truehdr: HDR 内容（transfer=%d primaries=%d）— 直通，不做上变换\n",
                    (int)mpi->params.color.transfer, (int)mpi->params.color.primaries);
            p->hdr_warned = true;
        }
        emit_status(f, p, "passthrough");
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (!p->opts->enable || p->failed) {
        emit_status(f, p, "passthrough");
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // 输入形态判定（TrueHDR 只吃 chunky RGBA8；见头注释）。
    const int subfmt = mpi->params.hw_subfmt;
    const bool in_rgba8  = (subfmt == IMGFMT_RGBA);
    const bool in_rgb10  = (subfmt == IMGFMT_X2BGR10 || subfmt == IMGFMT_X2RGB10);
    const bool in_yuv    = !vfx_imgfmt_is_rgb(subfmt);
    if (!in_rgba8 && !in_rgb10 && !in_yuv) {
        if (!p->fmt_warned) {
            MP_WARN(f, "truehdr: unsupported input subfmt=%d — passthrough\n", subfmt);
            p->fmt_warned = true;
        }
        emit_status(f, p, "passthrough");
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    const int w = mpi->w, h = mpi->h;
    p->video_w = w;
    p->video_h = h;

    if (!ensure_cuda(f, p, mpi)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    cuCtxPushCurrent(p->cuda_ref->ctx);

    double t0 = truehdr_now_ms();

    // ── 引擎（尺寸/参数不匹配则重建；失败 → 永久直通）──────────────────
    // 尺寸变化（换片/分辨率切换）→ 必须重建：图像缓冲与 tile 划分绑在 Load 上。
    if (p->loaded && (p->eng_w != w || p->eng_h != h))
        engine_destroy(p);
    // 参数变化 → SetU32 轻量更新（零重建，见 engine_params_apply）；失败/尺寸变化
    // → 重建。UI 滑块的节流拖动就走这条路。
    const bool need_rebuild = !p->loaded || !engine_params_apply(f, p);
    if (need_rebuild) {
        if (!engine_create(f, p, w, h)) {
            p->failed = true;
            if (!p->failed_warned) {
                MP_WARN(f, "truehdr: 上变换不可用 — 直通（不再重试）\n");
                p->failed_warned = true;
            }
            cuCtxPopCurrent(NULL);
            emit_status(f, p, "passthrough");
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }
    }

    if (!p->fmt_logged) {
        p->fmt_logged = true;
        MP_INFO(f, "truehdr: %dx%d subfmt=%d (bits=%d) → X2BGR10 HDR10 "
                "(primaries=%d transfer=%d max_luma=%d) luminance=%d debanding=%s\n",
                w, h, subfmt, vfx_bits_from_imgfmt(subfmt),
                (int)pl_color_space_hdr10.primaries, (int)pl_color_space_hdr10.transfer,
                p->e_luminance, p->e_luminance,
                p->e_debanding_off ? "OFF(diagnostic)" : "on");
    }

    // ── 输入 → in_img（RGBA8）───────────────────────────────────────────
    bool in_ok = true;
    if (in_yuv) {
        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(subfmt);
        int bd = desc.bpp[0];
        enum yuv_matrix mx = matrix_from_repr(&mpi->params);
        enum yuv_range  rn = range_from_levels(mpi->params.repr.levels);
        if (!ensure_yuv_converter(f, p, bd, mx, rn)) {
            in_ok = false;
        } else if (!yuv_to_rgba_convert(&p->yuv_conv,
                                        mpi->planes[0], mpi->stride[0],
                                        mpi->planes[1], mpi->stride[1],
                                        w, h, p->in_img.pixels, p->in_img.pitch,
                                        p->cuda_stream)) {
            MP_ERR(f, "truehdr: yuv_to_rgba_convert failed\n");
            in_ok = false;
        }
    } else if (in_rgba8) {
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice     = (CUdeviceptr)mpi->planes[0];
        copy.srcPitch      = (size_t)mpi->stride[0];
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice     = (CUdeviceptr)p->in_img.pixels;
        copy.dstPitch      = (size_t)p->in_img.pitch;
        copy.WidthInBytes  = (size_t)w * 4;
        copy.Height        = (size_t)h;
        in_ok = (cuMemcpy2DAsync(&copy, p->cuda_stream) == CUDA_SUCCESS);
        if (!in_ok) MP_ERR(f, "truehdr: input D2D copy failed\n");
    } else {
        // 10-bit 容器的 **SDR** 源（X2BGR10）：先落到 RGBA8。
        if (!p->in10_alloc) {
            int r = pfn_NvCVImage_Alloc(&p->in10_img, w, h, NVCV_RGB10A2,
                                        NVCV_P32, NVCV_CHUNKY, NVCV_GPU, 32);
            if (r != 0) {
                MP_ERR(f, "truehdr: in10 alloc failed (%d)\n", r);
                in_ok = false;
            } else {
                p->in10_alloc = true;
            }
        }
        if (in_ok) {
            CUDA_MEMCPY2D copy = {0};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice     = (CUdeviceptr)mpi->planes[0];
            copy.srcPitch      = (size_t)mpi->stride[0];
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice     = (CUdeviceptr)p->in10_img.pixels;
            copy.dstPitch      = (size_t)p->in10_img.pitch;
            copy.WidthInBytes  = (size_t)w * 4;
            copy.Height        = (size_t)h;
            if (cuMemcpy2DAsync(&copy, p->cuda_stream) != CUDA_SUCCESS) {
                MP_ERR(f, "truehdr: input10 D2D copy failed\n");
                in_ok = false;
            } else if (pfn_NvCVImage_Transfer(&p->in10_img, &p->in_img, 1.0f,
                                              p->cuda_stream, NULL) != 0) {
                MP_ERR(f, "truehdr: 10-bit→8-bit transfer failed\n");
                in_ok = false;
            }
        }
    }

    if (!in_ok) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        cuCtxPopCurrent(NULL);
        return;
    }

    // ── 上变换 ──────────────────────────────────────────────────────────
    if (pfn_NvVFX_Run(p->effect, 0) != 0) {
        MP_ERR(f, "truehdr: NvVFX_Run failed — 直通并停用\n");
        p->failed = true;
        cuCtxPopCurrent(NULL);
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    if (!ensure_out_buf(f, p, p->out_img.pitch, h)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        cuCtxPopCurrent(NULL);
        return;
    }

    {
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice     = (CUdeviceptr)p->out_img.pixels;
        copy.srcPitch      = (size_t)p->out_img.pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice     = p->out_buf;
        copy.dstPitch      = (size_t)p->out_pitch;
        copy.WidthInBytes  = (size_t)w * 4;
        copy.Height        = (size_t)h;
        if (cuMemcpy2DAsync(&copy, p->cuda_stream) != CUDA_SUCCESS) {
            MP_ERR(f, "truehdr: output D2D copy failed\n");
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            cuCtxPopCurrent(NULL);
            return;
        }
        cuStreamSynchronize(p->cuda_stream);
    }

    // status 数值证据（读回 32 点，PQ → nit）
    sample_output_nits(p, &p->last_med_nit, &p->last_peak_nit);

    struct mp_image *out = make_output(f, p, mpi, w, h);
    if (!out) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        cuCtxPopCurrent(NULL);
        return;
    }

    MP_DBG(f, "truehdr: frame %d %dx%d → X2BGR10 PQ\n", p->frame_count, w, h);
    talloc_free(mpi);

    p->status_cost_ms += truehdr_now_ms() - t0;
    p->status_frames++;
    emit_status(f, p, "active");

    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
    cuCtxPopCurrent(NULL);
}

// ── truehdr_command ────────────────────────────────────────────────────────
// 客户端经 `vf-command truehdr <param> <value>` 下发（派发名 = 过滤器名）：
//   enable 0|1|off|on     上变换开关（真值由客户端 HDR 策略决定；HDR 内容仍自守卫）
//   luminance <400..2000> 目标显示峰值 nit（客户端下面板物理峰值）
//   saturation 0..200 / contrast 0..200 / middle-gray 10..100
//   —— 范围/默认全部照 libnvVFXTrueHDR.so 内的参数文档串（该 effect 只有这 5 个
//      可调参数 + BatchSize(=1)；没有质量档/色域/transfer 类旋钮）
// 参数只改私有 opts，f_process 下一帧比对后走 SetU32 热更新（零重建）。

static bool parse_bool_arg(const char *arg, int *out)
{
    if (!arg) return false;
    if (!strcmp(arg, "1") || !strcmp(arg, "on") || !strcmp(arg, "yes")) { *out = 1; return true; }
    if (!strcmp(arg, "0") || !strcmp(arg, "off") || !strcmp(arg, "no")) { *out = 0; return true; }
    return false;
}

static bool parse_int_arg(const char *arg, int lo, int hi, int *out)
{
    if (!arg || !*arg) return false;
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (!end || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static bool truehdr_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || !cmd->arg)
        return false;

    struct vf_truehdr_opts *o = p->opts;
    if (!strcmp(cmd->cmd, "enable")) {
        int v;
        if (!parse_bool_arg(cmd->arg, &v)) return false;
        if (o->enable != v) {
            MP_INFO(f, "truehdr: enable %d -> %d (vf-command)\n", o->enable, v);
            o->enable = v;
        }
        return true;
    }
    if (!strcmp(cmd->cmd, "luminance")) {
        int v;
        if (!parse_int_arg(cmd->arg, 400, 2000, &v)) return false;
        if (o->luminance != v) {
            MP_INFO(f, "truehdr: luminance %d -> %d nit (vf-command)\n", o->luminance, v);
            o->luminance = v;
        }
        return true;
    }
    if (!strcmp(cmd->cmd, "saturation")) {
        int v;
        if (!parse_int_arg(cmd->arg, 0, 200, &v)) return false;
        if (o->saturation != v) {
            MP_INFO(f, "truehdr: saturation %d -> %d (vf-command)\n", o->saturation, v);
            o->saturation = v;
        }
        return true;
    }
    if (!strcmp(cmd->cmd, "contrast")) {
        int v;
        if (!parse_int_arg(cmd->arg, 0, 200, &v)) return false;
        if (o->contrast != v) {
            MP_INFO(f, "truehdr: contrast %d -> %d (vf-command)\n", o->contrast, v);
            o->contrast = v;
        }
        return true;
    }
    if (!strcmp(cmd->cmd, "middle-gray")) {
        int v;
        if (!parse_int_arg(cmd->arg, 10, 100, &v)) return false;
        if (o->middle_gray != v) {
            MP_INFO(f, "truehdr: middle-gray %d -> %d (vf-command)\n", o->middle_gray, v);
            o->middle_gray = v;
        }
        return true;
    }
    // debanding-off 只作诊断 A/B（默认恒 0 = 抗色带开，铁律 6）
    if (!strcmp(cmd->cmd, "debanding-off")) {
        int v;
        if (!parse_bool_arg(cmd->arg, &v)) return false;
        if (o->debanding_off != v) {
            MP_WARN(f, "truehdr: debanding-off %d -> %d (诊断用；默认恒 0)\n",
                    o->debanding_off, v);
            o->debanding_off = v;
        }
        return true;
    }
    return false;
}

// ── f_reset / f_destroy ────────────────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->cuda_ref && p->cuda_ref->ctx) {
        cuCtxPushCurrent(p->cuda_ref->ctx);
        engine_destroy(p);
        cuCtxPopCurrent(NULL);
    }
    p->frame_count = 0;
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    bool ctx_pushed = false;
    if (p->cuda_ref && p->cuda_ref->ctx)
        ctx_pushed = (cuCtxPushCurrent(p->cuda_ref->ctx) == CUDA_SUCCESS);

    engine_destroy(p);   // 含 in10_img（10-bit 中转图）
    if (p->yuv_conv_ready) {
        yuv_to_rgba_destroy(&p->yuv_conv);
        p->yuv_conv_ready = false;
    }
    if (p->out_buf) {
        if (ctx_pushed) cuMemFree(p->out_buf);
        p->out_buf = 0;
        p->out_buf_size = 0;
    }
    if (p->cuda_init_done && ctx_pushed)
        cuStreamDestroy(p->cuda_stream);
    p->cuda_stream = NULL;

    if (ctx_pushed)
        cuCtxPopCurrent(NULL);

    // frames/device 载体释放必须发生在无 push 状态（uninit 可能 cuCtxDestroy）
    if (p->av_hw_frames) { av_buffer_unref(&p->av_hw_frames); p->av_hw_frames = NULL; }
    if (p->av_hw_device) { av_buffer_unref(&p->av_hw_device); p->av_hw_device = NULL; }

    if (p->cuda_ref) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
    }
    p->cuda_init_done = false;
}

// ── Filter info / create / registration ────────────────────────────────────

static const struct mp_filter_info vf_truehdr_filter = {
    .name      = "truehdr",
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = truehdr_command,
};

static struct mp_filter *vf_truehdr_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_truehdr_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);

    if (!vfx_load_libraries()) {
        MP_ERR(f, "truehdr: VFX 库加载失败 — 直通\n");
        p->failed = true;
        p->failed_warned = true;
    }
    return f;
}

const struct mp_user_filter_entry vf_truehdr = {
    .desc = {
        .name          = "truehdr",
        .description   = "SDR→HDR10 AI up-conversion via NVIDIA VFX SDK TrueHDR",
        .priv_size     = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_truehdr_opts_def,
        .options       = vf_opts_fields,
    },
    .create = vf_truehdr_create,
};
