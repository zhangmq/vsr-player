/*
 * vf_vsr.c -- Video Super Resolution filter via NVIDIA VFX SDK
 *
 * 输入约定：只处理 CUDA 帧（SW→HW 一致化由链上的 vf_hwup 负责，见其头注释
 * "rife/vsr 不再需要各自的 SW 支持"）。非 CUDA 帧一律直通——早期自带的软解
 * 上传分支已移除（issue #12 缺陷 6：它每帧重建 CUDA context，导致 VSR 引擎的
 * in_pixels 失效 → 第二帧段错误）。
 * HW CUDA/RGBA: D2D 拷入引擎输入缓冲
 * HW CUDA/YUV:  yuv_to_rgba D2D
 *
 * Filter options:
 *   scale:   off | auto | 2 | 3 | 4            (default: auto)
 *   denoise: off | low | medium | high | ultra  (default: off)
 *   quality: low | medium | high | ultra        (default: high)
 */

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "video/image_writer.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "common/common.h"
#include "mpv/client.h"   // struct mpv_node（自定义 option type 的 set/get）

#include <libavcodec/avcodec.h>

#include <cuda.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include "vsr_internal.h"
#include "cuda_shared.h"

#include <time.h>

static double vsr_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ── Options ────────────────────────────────────────────────────────────────

struct vf_vsr_opts {
    float scale;   // -1=off, 0=auto, 倍率（SDK 支持集：4/3, 1.5, 2, 3, 4）
    int quality;   // 1-4
    int denoise;   // -1=off, 8-11
};

// ── scale 选项类型（off/auto 字符串 + 数值/ratio 浮点）──────────────────
// mpv 0.41 无 OPT_CHOICE_OR_FLOAT：自定义 m_option_type 使字符串
// "off"/"auto"（独立 mpv-vsr CLI 直接传 `--vf=vsr:scale=auto`）与
// 数字/ratio 等价。数值路径对齐上游 parse_numeric 语义（含
// "4:3"/"4/3" ratio，SDK 支持非整数倍 4/3x、1.5x，vfx_test6 实测）。
// print 统一输出数值（off/auto 仅是输入别名）。

static int vsr_scale_clamp(const m_option_t *opt, double *val)
{
    double v = *val;
    int r = 0;
    if (opt->min < opt->max) {
        if (v > opt->max) { v = opt->max; r = M_OPT_OUT_OF_RANGE; }
        if (v < opt->min) { v = opt->min; r = M_OPT_OUT_OF_RANGE; }
    }
    if (!isfinite(v) && v != opt->max && v != opt->min) {
        v = opt->min;
        r = M_OPT_OUT_OF_RANGE;
    }
    *val = v;
    return r;
}

static int vsr_scale_parse(struct mp_log *log, const m_option_t *opt,
                           struct bstr name, struct bstr param, void *dst)
{
    double v;
    if (bstr_equals0(param, "off")) {
        v = -1.0;
    } else if (bstr_equals0(param, "auto")) {
        v = 0.0;
    } else {
        if (param.len == 0)
            return M_OPT_MISSING_PARAM;
        struct bstr rest;
        v = bstrtod(param, &rest);
        if (bstr_eatstart0(&rest, ":") || bstr_eatstart0(&rest, "/"))
            v /= bstrtod(rest, &rest);
        if (rest.len) {
            mp_err(log, "The %.*s option must be a floating point number or a "
                   "ratio (numerator[:/]denominator): %.*s\n",
                   BSTR_P(name), BSTR_P(param));
            return M_OPT_INVALID;
        }
    }
    if (vsr_scale_clamp(opt, &v) < 0) {
        mp_err(log, "The %.*s option is out of range: %.*s\n",
               BSTR_P(name), BSTR_P(param));
        return M_OPT_OUT_OF_RANGE;
    }
    if (dst)
        *(float *)dst = (float)v;
    return 1;
}

static char *vsr_scale_print(const m_option_t *opt, const void *val)
{
    return talloc_asprintf(NULL, "%f", *(const float *)val);
}

static void vsr_scale_copy(const m_option_t *opt, void *dst, const void *src)
{
    if (dst && src)
        memcpy(dst, src, sizeof(float));
}

static int vsr_scale_set(const m_option_t *opt, void *dst, struct mpv_node *src)
{
    double v;
    if (src->format == MPV_FORMAT_INT64)
        v = (double)src->u.int64;
    else if (src->format == MPV_FORMAT_DOUBLE)
        v = src->u.double_;
    else if (src->format == MPV_FORMAT_FLAG)
        v = src->u.flag ? 1.0 : 0.0;
    else
        return M_OPT_INVALID;
    if (vsr_scale_clamp(opt, &v) < 0)
        return M_OPT_OUT_OF_RANGE;
    *(float *)dst = (float)v;
    return 1;
}

static int vsr_scale_get(const m_option_t *opt, void *ta_parent,
                         struct mpv_node *dst, void *src)
{
    dst->format = MPV_FORMAT_DOUBLE;
    dst->u.double_ = *(const float *)src;
    return 1;
}

static bool vsr_scale_equal(const m_option_t *opt, void *a, void *b)
{
    return memcmp(a, b, sizeof(float)) == 0;
}

static const m_option_type_t m_option_type_vsr_scale = {
    .name  = "Scale",
    .flags = M_OPT_TYPE_USES_RANGE,
    .size  = sizeof(float),
    .parse = vsr_scale_parse,
    .print = vsr_scale_print,
    .copy  = vsr_scale_copy,
    .set   = vsr_scale_set,
    .get   = vsr_scale_get,
    .equal = vsr_scale_equal,
};

#define OPT_BASE_STRUCT struct vf_vsr_opts

static const struct m_option vf_opts_fields[] = {
    {"scale",   OPT_TYPED_FIELD(m_option_type_vsr_scale, float, scale),
                M_RANGE(-1, 4)},
    {"denoise", OPT_CHOICE(denoise,
        {"off", -1}, {"low", 8}, {"medium", 9}, {"high", 10}, {"ultra", 11}),
        M_RANGE(-1, 11)},
    {"quality", OPT_CHOICE(quality,
        {"low", 1}, {"medium", 2}, {"high", 3}, {"ultra", 4}),
        M_RANGE(1, 4)},
    {0}
};

static const struct vf_vsr_opts vf_vsr_opts_def = {
    .scale   = 0,
    .quality = 3,
    .denoise = -1,
};

// ── Filter private state ───────────────────────────────────────────────────

/// CUDA context ownership — shared struct (cuda_shared.h), carried as the
/// opaque of each output frame's bufs[0]. Three modes:
///   - borrow upstream shared struct (hwup/rife output frames): acquire the
///     same struct — the context stays alive while ANY frame in the chain
///     references it (frames can outlive the filter: pin teardown frees
///     frames AFTER f_destroy ran — use-after-free otherwise)
///   - borrow hwdec context (decoder frames, opaque not ours): own struct +
///     ctx_holder (av_buffer_ref of the frame hwctx), released on last ref
///   - SW input fallback: self-created context (owned=true), destroyed on
///     last release
struct priv {
    struct vf_vsr_opts *opts;

    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    AVBufferRef *av_hw_device;   // SW path: CUDA device ctx (self-created)
    AVBufferRef *av_hw_frames;   // SW path: CUDA/RGBA frames ctx for output
    CUcontext    av_hw_frames_ctx;  // ctx av_hw_frames was created on

    struct vsr_context vsr;
    bool      vsr_configured;

    // yuv→rgba D2D
    struct yuv_to_rgba    yuv_conv;
    bool                  yuv_conv_ready;
    int                   yuv_conv_bd;
    int                   yuv_conv_bits;   // 输出位深（8/10）
    enum yuv_matrix       yuv_conv_matrix;
    enum yuv_range        yuv_conv_range;

    CUdeviceptr out_buf;
    size_t      out_buf_size;
    int         out_w, out_h, out_pitch;

    int video_w, video_h;
    float effective_scale;

    bool passthrough;
    bool warmup_done;
    int  frame_count;
    // vsr-status（每 30 帧）：处理耗时 + 频率
    double status_t0;
    int    status_frames;
    double status_cost_ms;
    int    status_emit;        // passthrough 分支输出计数（每 30 帧）
    bool   sw_warned;          // 软解输入直通只警告一次（同 vf_rife.sw_warned）
    // seek 目标前的帧直接丢弃（0 = 关闭）：mpv 的 hr-seek framedrop 只在
    // 解码器层按 packet 丢弃，某些流（packet pts 被判 broken 等）会把
    // 关键帧→目标之间的帧送进滤镜链；它们最终在 VO 被丢弃，跑 VSR 是白费
    //（实测一次 seek 白跑 ~28 帧 × 6 ms）。客户端发起绝对 seek 前用
    // `vf-command vsr seek-target <秒>` 告知目标，pts 小于目标的帧一律丢弃，
    // 标记 3 s 后自动过期（或下一次 seek 覆盖）。
    double skip_before_pts;
    double skip_expire_ms;     // 标记过期时刻（seek 失败时避免永久丢帧）
    int    skip_count;         // 本次标记丢弃了多少帧（诊断日志用）
    bool   skip_passed;        // 已越过目标（只让"reached"日志打一次）
};

// ── compute_adaptive_scale ─────────────────────────────────────────────────

static float compute_adaptive_scale(int phys_w, int phys_h,
                                   int video_w, int video_h)
{
    if (video_w <= 0 || video_h <= 0) return 1.0f;

    // 所需最小倍率：VSR 输出目标是覆盖**显示区域**（视频 aspect fit 到
    // 视口后的区域，letterbox/pillarbox 黑边不算需求）——显示区域/视频
    // = min(phys_w/video_w, phys_h/video_h)（两维中刚好适配的缩放）。
    // 取 max 是错的：宽银幕视频（如 3840×1600 在 2160p 视口）高度方向
    // 的 1.35 是 letterbox 黑边，非真实放大需求（实测 auto 误出 1.5×）。
    double need_w = (double)phys_w / video_w;
    double need_h = (double)phys_h / video_h;
    double need = (need_w < need_h) ? need_w : need_h;

    // 显示区域 ≤ 视频（无需放大）→ 1:1（passthrough / 降噪）
    if (need <= 1.0) return 1.0f;

    // SDK 支持集合中选最小满足者——非整数倍（4/3、1.5）可避免整数
    // ceil 的过采样（如 1366×768 窗口 + 720p：1.07 → 4/3x 而非 2x）。
    // 选中的倍率仍受 f_process 支持矩阵约束（4x≤540p 等）。
    static const float supported[] = {4.0f / 3.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        if (need <= supported[i]) return supported[i];
    return 4.0f;   // 超大窗口：最高倍率（超出矩阵上限时由 f_process 回退）
}

// ── resolve_scale ──────────────────────────────────────────────────────────

static float resolve_scale(struct mp_filter *f, struct priv *p,
                          int video_w, int video_h)
{
    if (p->opts->scale == -1) return 1.0f;  // denoise only: 1:1
    if (p->opts->scale > 0)  return p->opts->scale;

    // scale == 0: auto — requires render target size from VO
    // (window for wayland/x11 VOs, render target for vo_libmpv)
    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    int res[2] = {0};
    if (info && info->get_render_target_size)
        info->get_render_target_size(info, res);
    if (res[0] <= 0 || res[1] <= 0)
        return p->effective_scale;  // not yet available — keep current

    return compute_adaptive_scale(res[0], res[1], video_w, video_h);
}

// ── CUDA context setup ─────────────────────────────────────────────────────
// Single-context design (CUDA best practice): all filter work shares ONE
// driver context (multiple contexts in one process time-slice on the GPU and
// hung the device when vf_rife's TensorRT context ran concurrently with
// VSR). The context comes from three sources, all backed by the shared
// refcounted struct (cuda_shared.h):
//   1. upstream frames carry OUR shared struct (hwup output / rife output) →
//      acquire it — the ctx stays alive while any frame references it
//   2. hwdec decoder frames (opaque not ours) → borrower struct with
//      ctx_holder (frame hwctx ref)
//   3. software frames (no hwctx) → self-created context (owned)
//
// GPU serialization with vf_rife: each filter synchronizes its own stream at
// the end of processing. The mpv filter chain is host-serial (pull model),
// so rife's pair work and VSR's frame work alternate strictly on the GPU —
// no concurrent saturation, no device-level sync (which would also wait for
// the VO's stream-0 copies and can deadlock — Xid 109 lesson).

static bool ensure_cuda(struct mp_filter *f, struct priv *p,
                        struct mp_image *mpi)
{
    CUcontext borrow = mp_cuda_ctx_from_mpi(mpi);  // NULL = SW frame

    // fast path: binding unchanged. `owned` marks the struct's CREATOR
    // (destruction semantics), not the binding kind — a borrowed upstream
    // struct may be owned by hwup. Compare the driver context only.
    if (p->cuda_ref) {
        if (p->cuda_ref->ctx == borrow)
            return true;
    }

    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuInit failed (%d)\n", cr);
        return false;
    }

    // (re)bind: release the old context (frames in flight keep it alive via
    // their refs), then adopt the new one.
    // ⚠ 所有 CUDA 资源都绑在**创建它的 context** 上：引擎（CUmodule/engine 句柄）、
    // yuv_to_rgba 的 NVRTC module、本滤镜的流、out_buf。旧代码只销毁了流，
    // 切 hwdec（软解↔硬解，上游换 context）后仍复用旧引擎/旧 kernel →
    // cuLaunchKernel 返回 400（CUDA_ERROR_INVALID_VALUE）→ 本滤镜被 mpv 禁用
    // （2026-10-17 实测：切软解 VSR 立刻失败，切回硬解 VFG 也失败 → "插帧失效"）。
    // 因此必须在旧 context 内把它们全部作废，下帧按需在新 context 重建。
    if (p->cuda_ref) {
        CUcontext old = p->cuda_ref->ctx;
        if (old) cuCtxPushCurrent(old);
        if (p->vsr_configured) {
            vsr_destroy(&p->vsr);
            p->vsr_configured = false;
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
        MP_VERBOSE(f, "vsr: CUDA context changed — engine/converter reset\n");
    }

    struct mp_cuda_ctx_ref *shared = mp_cuda_ctx_ref_from_mpi(mpi);
    if (shared) {
        // 上游链（hwup/rife 输出）的共享结构：acquire 同一结构
        mp_cuda_ctx_ref_acquire(shared);
        p->cuda_ref = shared;
        MP_VERBOSE(f, "vsr: sharing upstream CUDA context\n");
    } else if (borrow) {
        // hwdec 帧（opaque 非本链共享结构）：自建借者结构 + ctx_holder
        p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
        p->cuda_ref->magic = MP_CUDA_CTX_REF_MAGIC;
        p->cuda_ref->ctx = borrow;
        p->cuda_ref->owned = false;
        p->cuda_ref->ctx_holder = av_buffer_ref(mpi->hwctx);
        if (!p->cuda_ref->ctx_holder) {
            MP_ERR(f, "vsr: failed to hold hwdec frame ref\n");
            free(p->cuda_ref);
            p->cuda_ref = NULL;
            return false;
        }
        atomic_init(&p->cuda_ref->refs, 1);
        MP_VERBOSE(f, "vsr: sharing hwdec CUDA context\n");
    } else {
        // 软解帧已在 f_process 直通（契约：SW→HW 由 vf_hwup 负责），走到这里说明
        // 上游状态异常——不再自建 context（旧实现每帧重建 → 引擎 in_pixels 失效，
        // issue #12 缺陷 6）。
        MP_ERR(f, "vsr: no CUDA context from input (software frame past passthrough)\n");
        return false;
    }

    cuCtxPushCurrent(p->cuda_ref->ctx);
    // 必须是**阻塞流**（官方 "Setting Up the CUDA Stream" 规定的 cudaStreamCreate
    // 默认语义；NvVFX_CudaStreamCreate 反汇编可见就是一句 cudaStreamCreate）：
    // VFX 1.3 内部有固定挂在 legacy 默认流上的环节（1.1 无此机制），非阻塞流与
    // legacy 流**没有隐含顺序** → 引擎会读到尚未落地的输入，表现为每个输出帧装的
    // 是上一帧，并叠加非确定性重复/跳源帧（build/scratch/probe/BLOCKING_REPORT.md、
    // build/scratch/player-lag/BEFORE.md）。阻塞流与 legacy 流隐含同步，整条链
    //（色彩转换 → 输入搬运 → 超分 → 输出搬运）都在本流上按序完成。
    // ⚠️ 输出必须在本流上读回；换别的流读需要自行同步。
    cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_DEFAULT);
    cuCtxPopCurrent(NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuStreamCreate failed (%d)\n", cr);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        return false;
    }

    p->cuda_init_done = true;
    return true;
}

// ── AVHWFramesContext for CUDA/RGBA output ─────────────────────────────────
// Output frames carry a self-created RGBA frames ctx (NOT a borrow of the
// input's NV12/P010 hwctx): hwdownload parses frame data per hwctx sw_format
// — an NV12 hwctx under an RGBA frame crashes on the SW-input download path
// (no hwdec → VO lacks CUDA interop → chain-tail hwdownload). Shared helper
// in cuda_shared.h (mp_cuda_ensure_frames).

static bool ensure_av_hw_frames(struct mp_filter *f, struct priv *p,
                                 int w, int h, int bits)
{
    if (!p->cuda_init_done) return false;
    bool had = p->av_hw_frames != NULL;
    // ⚠ hw_frames_ctx 的 sw_format 恒用 AV_PIX_FMT_RGBA：FFmpeg 的 CUDA
    // hwframes 不支持 x2rgb10（build/scratch/hwf_test.c 实测：
    // rgba/bgra/rgb0 ok，x2rgb10le "Pixel format not supported"）。
    // 两者都是 4 字节/px、同 stride，所以缓冲语义一致；10-bit 的**真实
    // 格式**由帧参数 hw_subfmt=IMGFMT_X2RGB10 承载（VO 的 CUDA interop
    // 按 hw_subfmt 选 VkFormat，拷贝宽度用 pixel_size=4 —— hwdec_cuda.c:
    // 255 "plane_w * format->pixel_size"，与 sw_format 无关）。
    // 唯一受影响的是 hwdownload 软路径（本应用不会走）：那里会按 sw_format
    // 解析成 RGBA 字节，10-bit 数据会被当 8-bit 显示。
    if (!mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx, AV_PIX_FMT_RGBA,
                               w, h, &p->av_hw_device, &p->av_hw_frames,
                               &p->av_hw_frames_ctx))
        return false;
    if (!had)
        MP_VERBOSE(f, "vsr: CUDA/4Bpp hw_frames_ctx %dx%d (%d-bit data)\n",
                   w, h, bits);
    return true;
}

// ── VSR init / reconfigure ─────────────────────────────────────────────────

static bool ensure_vsr(struct mp_filter *f, struct priv *p,
                        int in_w, int in_h, int out_w, int out_h,
                        int quality, int bits)
{
    if (p->vsr_configured && p->vsr.bits != bits) {
        // 位深变化（8↔10）必须重建：ImageEncodingMode + 图像格式都绑在
        // Load 上，不能热更新。
        MP_INFO(f, "vsr: bit depth %d -> %d (engine rebuild)\n",
                p->vsr.bits, bits);
        vsr_destroy(&p->vsr);
        p->vsr_configured = false;
    }
    if (p->vsr_configured) {
        // ── 引擎存在：优先轻量热更新，仅输入变化才重建 ──────────────
        // 尺寸全等 → quality 轻量（vfx_test5：SetU32 生效）。
        // 输入不变仅输出变 → vsr_set_output 换输出缓冲（vfx_outchange_
        // test + 播放器帧号 dump：PSNR 999dB 逐字节一致）——全屏切换
        //（输入不变）免引擎重建。
        // 输入变化 → 重建（真实播放器换文件时 mpv 重建整个 filter 链，
        // 引擎必然重建 + warmup；此处为同 filter 内输入变化的防御路径）。
        bool in_same = (p->vsr.in_w == in_w && p->vsr.in_h == in_h);
        bool out_same = (p->vsr.out_w == out_w && p->vsr.out_h == out_h);
        if (in_same && out_same) {
            if (p->vsr.quality != quality) {
                if (!vsr_set_quality(&p->vsr, quality))
                    return false;
            }
            return true;
        }
        if (in_same) {
            // 仅输出尺寸变化：SetImage 换输出缓冲热更新（免重建，
            // vsr_set_output 质量验证结论——PSNR 999dB 逐字节一致）。
            if (!vsr_set_output(&p->vsr, out_w, out_h))
                return false;
            if (p->vsr.quality != quality) {
                if (!vsr_set_quality(&p->vsr, quality))
                    return false;
            }
            return true;
        } else {
            vsr_destroy(&p->vsr);
            p->vsr_configured = false;
        }
    }

    // 引擎不存在（filter 新建 / 尺寸变化重建 / passthrough 回切）：init
    MP_INFO(f, "vsr: init %dx%d -> %dx%d quality=%d bits=%d\n",
               in_w, in_h, out_w, out_h, quality, bits);
    if (!vsr_init(&p->vsr, in_w, in_h, out_w, out_h, quality, bits,
                  p->cuda_stream)) {
        MP_ERR(f, "vsr_init failed\n");
        return false;
    }
    p->vsr_configured = true;

    if (!p->warmup_done) {
        if (!vsr_warmup(&p->vsr, p->cuda_stream)) {
            MP_ERR(f, "vsr_warmup failed\n");
            return false;
        }
        p->warmup_done = true;
        MP_VERBOSE(f, "vsr: warmup complete\n");
    }
    return true;
}

// ── Allocate/realloc owned output buffer ───────────────────────────────────

static bool ensure_out_buf(struct mp_filter *f, struct priv *p,
                            int out_w, int out_h, int out_pitch)
{
    size_t needed = (size_t)out_pitch * out_h;
    if (p->out_buf && p->out_buf_size >= needed) {
        p->out_w     = out_w;
        p->out_h     = out_h;
        p->out_pitch = out_pitch;
        return true;
    }

    if (p->out_buf) {
        cuMemFree(p->out_buf);
        p->out_buf = 0;
        p->out_buf_size = 0;
    }

    CUresult cr = cuMemAlloc(&p->out_buf, needed);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuMemAlloc(out) failed (%d)\n", cr);
        return false;
    }
    p->out_buf_size = needed;
    p->out_w     = out_w;
    p->out_h     = out_h;
    p->out_pitch = out_pitch;
    return true;
}

// ── AVBufferRef free callback for CUDA memory ──────────────────────────────

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
    // No stream sync here. The frame is only released after the VO replaced
    // it with a newer render (retained-frame semantics), so the VO's map
    // copies of this buffer completed before we get here; vf_vsr's own copy
    // is drained by the per-frame stream sync. A cuStreamSynchronize(0)
    // would now wait on the SHARED context's stream 0 — which carries the
    // render thread's map copies — throttling the video thread to the
    // render loop's pace (GPU drops to ~idle, measured in benchmark).
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        mp_cuda_ctx_ref_release(r);
}

// ── yuv→rgba converter lifecycle ───────────────────────────────────────────

static bool ensure_yuv_converter(struct mp_filter *f, struct priv *p,
                                  int bit_depth, enum yuv_matrix matrix,
                                  enum yuv_range range, int out_bits,
                                  const struct mp_image_params *src)
{
    if (p->yuv_conv_ready &&
        p->yuv_conv_bd == bit_depth &&
        p->yuv_conv_bits == out_bits &&
        p->yuv_conv_matrix == matrix &&
        p->yuv_conv_range == range)
        return true;

    if (p->yuv_conv_ready)
        yuv_to_rgba_destroy(&p->yuv_conv);

    // HDR 上屏调试：把 YUV→RGB 用的矩阵/范围与源帧色彩元数据打出来
    //（矩阵取错 = BT.709 系数解 BT.2020 → 偏红，实测怀疑点）。
    MP_INFO(f, "yuv->rgb: bd=%d out=%d matrix=%d range=%d | src sys=%d levels=%d "
            "primaries=%d transfer=%d\n", bit_depth, out_bits, (int)matrix, (int)range,
            (int)src->repr.sys, (int)src->repr.levels,
            (int)src->color.primaries, (int)src->color.transfer);
    if (!yuv_to_rgba_init(&p->yuv_conv, bit_depth, matrix, range, out_bits)) {
        MP_ERR(f, "yuv_to_rgba_init failed (bd=%d out=%d)\n", bit_depth, out_bits);
        p->yuv_conv_ready = false;
        return false;
    }

    p->yuv_conv_bd = bit_depth;
    p->yuv_conv_bits = out_bits;
    p->yuv_conv_matrix = matrix;
    p->yuv_conv_range = range;
    p->yuv_conv_ready = true;
    return true;
}

// ── Color matrix helpers（matrix_from_repr/range_from_levels 共享实现
//    在 yuv_to_rgba.h——单一事实源）──────────────────────────────────

// ── f_process ──────────────────────────────────────────────────────────────

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame;
    struct mp_image *mpi;

    // seek 目标前的帧（关键帧→目标）直接丢弃，不跑引擎、不写输出。
    // ⚠ 必须在**同一次 process() 里连续拉取**（drop loop）：早先每丢一帧就
    // mark_progress + return、让 filter graph 再调一次本函数，实测每帧
    // ~0.3 ms 框架开销；长 GOP 素材一次 seek 可达 1500 帧（25 s @60fps）
    // → 450 ms。循环内丢弃后每帧只剩解码成本（~0.1 ms）。
    // ⚠ marker **不在首个 >= 目标的帧上清除**：mpv 在一次 seek 里会先喂目标
    // 之前的帧、再在链 reset 之后**又喂一遍**（实测首个达标帧之后 0–100 ms
    // 内跟来 1–2 s 的量，即 60–130 帧）。marker 早清 → 这一批重新进引擎
    //（2.8 ms/帧）→ 单次 seek 多 200–400 ms，正是按住方向键"偶发特别慢"
    // 的另一半来源。所以 marker 一直留到过期：它只丢弃 pts < 目标的帧，
    // 正常播放零影响（每帧只多一次比较）。
    for (;;) {
        if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
            return;
        frame = mp_pin_out_read(f->ppins[0]);

        if (mp_frame_is_signaling(frame)) {
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }

        if (frame.type != MP_FRAME_VIDEO) {
            MP_ERR(f, "vsr: non-video frame received\n");
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            return;
        }

        mpi = frame.data;
        if (p->skip_before_pts <= 0.0)
            break;

        // 兜底：marker 设了但一直没有 >= 目标的帧（seek 失败/直播流）→ 3 s 后
        // 自动失效，避免后续正常播放的帧被一直丢掉。
        if (vsr_now_ms() > p->skip_expire_ms) {
            if (p->skip_count > 0)
                MP_VERBOSE(f, "vsr: seek-target expired after %d skipped frames\n",
                           p->skip_count);
            p->skip_before_pts = 0.0;
            p->skip_count = 0;
            p->skip_passed = false;
            break;
        }

        if (mpi->pts != MP_NOPTS_VALUE && mpi->pts < p->skip_before_pts - 0.005) {
            p->skip_count++;
            mp_frame_unref(&frame);
            continue;
        }

        // 首个 >= 目标的帧（或目标之后的迟到帧）：照常处理，marker 继续留着。
        if (!p->skip_passed) {
            p->skip_passed = true;
            MP_VERBOSE(f, "vsr: seek-target reached, skipped %d frames\n",
                       p->skip_count);
        }
        break;
    }

    p->frame_count++;
    double t0 = vsr_now_ms();   // 处理计时起点

    // 通路位深：源 subfmt 是 10-bit 打包 RGB（上游 VFG）或源位深 >8
    //（P010/P016）→ 10-bit 通路（VFX ImageEncodingMode=1 + RGB10A2）；
    // 否则维持历史 8-bit 行为（逐字节不变，SDR 8-bit 内容零回归）。
    int bits = (mpi->imgfmt == IMGFMT_CUDA)
                   ? vfx_bits_from_imgfmt(mpi->params.hw_subfmt)
                   : VFX_BITS_8;

    // Scale/resolution detection
    int video_w = mpi->w;
    int video_h = mpi->h;
    if (video_w != p->video_w || video_h != p->video_h) {
        p->video_w = video_w;
        p->video_h = video_h;
        p->warmup_done = false;
    }
    float new_scale = resolve_scale(f, p, video_w, video_h);
    float eff = new_scale;
    // 无倍率输入保护：请求倍率直接生效。引擎实际硬限制 = 输出 ≤8K
    //（实测 1080p 4×→7680×4320 成功），超出引擎能力的请求由引擎侧
    // 处理（NvVFX 错误路径）。auto（resolve_scale）结果同样直接生效。
    int out_w = lrintf(video_w * eff);
    int out_h = lrintf(video_h * eff);

    if (fabsf(eff - p->effective_scale) > 0.001f) {
        MP_INFO(f, "vsr: scale changed %.2f -> %.2f (video %dx%d)\n",
                p->effective_scale, eff, video_w, video_h);
        p->effective_scale = eff;
        // 尺寸变化由 ensure_vsr 重建引擎（SDK 实验：SetImage 换尺寸
        // 输出错乱，tile 切分与 Load 时尺寸绑定）。
    }

    // ── Passthrough — re-evaluated per-frame (dynamic effective_scale) ──
    // scale=-1 时 resolve_scale 返回 1（降噪专用 1:1）——effective_scale==1
    // 已覆盖；denoise 开启时不得 passthrough（1:1 降噪模式仍要跑 VSR）。
    // 非 CUDA 帧（没有 hwup 的软解输入）同样直通——契约见 vf_hwup.c 头注释：
    // "使下游 rife/vsr 只处理 HW 路径（rife/vsr 不再需要各自的 SW 支持）"。
    // vf_vsr 早期自带的软解上传分支是那次重构的遗留：它在 ensure_cuda 里每帧重建
    // context，而 VSR 引擎的 in_pixels 仍指向旧 context 的显存 → 第二帧即段错误
    //（issue #12 缺陷 6：实测 exit 139，SEGV 落在 cuMemcpy2DAsync）。按契约与
    // vf_rife 同款处理：一次性 WARN + 直通，不初始化引擎、不碰 CUDA。
    const bool sw_input = (mpi->imgfmt != IMGFMT_CUDA);
    if (sw_input && !p->sw_warned) {
        MP_WARN(f, "vsr: software frames — passthrough (prepend @hwup for VSR)\n");
        p->sw_warned = true;
    }
    p->passthrough = sw_input
                  || (fabsf(p->effective_scale - 1.0f) < 0.001f && p->opts->denoise == -1);
    if (p->passthrough) {
        if (p->vsr_configured) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            vsr_destroy(&p->vsr);
            cuCtxPopCurrent(NULL);
            p->vsr_configured = false;
            MP_INFO(f, "vsr: switching to passthrough (scale=%.2f, denoise=off)\n",
                    p->effective_scale);
        }
        // passthrough 也输出状态行（mode=passthrough）——OSD 的 VSR-ST
        // 行由 vsr-status 驱动，不输出会残留激活时的 cost（陈旧显示）。
        // 低频（每 30 帧），与激活路径同节奏。
        if (++p->status_emit >= 30) {
            p->status_emit = 0;
            mp_msg(f->log, MSGL_STATUS,
                   "vsr-status: mode=passthrough scale=%.2f\n",
                   p->effective_scale);
        }
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    if (!ensure_cuda(f, p, mpi)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    cuCtxPushCurrent(p->cuda_ref->ctx);

    if (!ensure_vsr(f, p, video_w, video_h, out_w, out_h, p->opts->quality, bits)) {
        // 首次失败：销毁引擎状态重试一次（瞬时失败自愈，如 GPU 忙）。
        if (p->vsr_configured) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            vsr_destroy(&p->vsr);
            cuCtxPopCurrent(NULL);
            p->vsr_configured = false;
        }
        if (!ensure_vsr(f, p, video_w, video_h, out_w, out_h,
                        p->opts->quality, bits)) {
            MP_ERR(f, "vsr: init failed after retry\n");
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            cuCtxPopCurrent(NULL);
            return;
        }
    }

    // Timing

    // ── Thin shim: write RGBA directly to vsr.in_pixels ──────────────────
    bool is_hw = (mpi->imgfmt == IMGFMT_CUDA);
    int vsr_in_pitch = p->vsr.in_pitch;

    if (is_hw && !vfx_imgfmt_is_rgb(mpi->params.hw_subfmt)) {
        // HW YUV → D2D to vsr.in_pixels
        int subfmt = mpi->params.hw_subfmt;
        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(subfmt);
        int bd = desc.bpp[0];
        enum yuv_matrix mx = matrix_from_repr(&mpi->params);
        enum yuv_range  rn = range_from_levels(mpi->params.repr.levels);
        if (!ensure_yuv_converter(f, p, bd, mx, rn, bits, &mpi->params)) {
            mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
        if (!yuv_to_rgba_convert(&p->yuv_conv,
                                  mpi->planes[0], mpi->stride[0],
                                  mpi->planes[1], mpi->stride[1],
                                  video_w, video_h,
                                  p->vsr.in_pixels, vsr_in_pitch,
                                  p->cuda_stream)) {
            MP_ERR(f, "vsr: yuv_to_rgba_convert failed\n");
            mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
    } else {
        // HW CUDA/RGBA: copy D2D to vsr.in_pixels (different stride possible)
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice     = (CUdeviceptr)mpi->planes[0];
        copy.srcPitch      = (size_t)mpi->stride[0];
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice     = (CUdeviceptr)p->vsr.in_pixels;
        copy.dstPitch      = (size_t)vsr_in_pitch;
        copy.WidthInBytes  = (size_t)video_w * 4;
        copy.Height        = (size_t)video_h;
        if (cuMemcpy2DAsync(&copy, p->cuda_stream) != CUDA_SUCCESS) {
            mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
    }

    // ── VSR process ──────────────────────────────────────────────────────
    void *vsr_out_ptr = NULL;
    int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
    if (!vsr_process(&p->vsr, p->cuda_stream,
                      &vsr_out_ptr, &vsr_out_w, &vsr_out_h, &vsr_out_pitch)) {
        MP_ERR(f, "vsr: vsr_process failed\n");
        mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
    }

    if (!ensure_out_buf(f, p, vsr_out_w, vsr_out_h, vsr_out_pitch)) {
        mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
    }

    {
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice     = (CUdeviceptr)vsr_out_ptr;
        copy.srcPitch      = (size_t)vsr_out_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice     = p->out_buf;
        copy.dstPitch      = (size_t)vsr_out_pitch;
        copy.WidthInBytes  = (size_t)vsr_out_w * 4;
        copy.Height        = (size_t)vsr_out_h;
        if (cuMemcpy2DAsync(&copy, p->cuda_stream) != CUDA_SUCCESS) {
            mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
        cuStreamSynchronize(p->cuda_stream);
    }

    // ── Output ───────────────────────────────────────────────────────────
    // Each output frame takes a reference on the CUDA context: the frame
    // can outlive the filter (pin teardown frees frames after f_destroy),
    // so free_cuda_buf must never touch a destroyed context.
    //
    // hwctx = 自建 CUDA/RGBA frames（输出尺寸，当前 ctx 上）——hwctx 的
    // sw_format 必须与输出帧 subfmt（RGBA）一致：软解场景 VO 无 CUDA
    // interop → hwdownload → av_hwframe_transfer_data 按 hwctx sw_format
    // 解析帧数据，借用 NV12 hwctx（hwup/hwdec 的）解析 RGBA 帧错乱崩溃
    //（mp_image_hw_download assert，实测）。HW/SW 输入输出段同构。
    mp_cuda_ctx_ref_acquire(p->cuda_ref);
    struct mp_image *out;
    if (!ensure_av_hw_frames(f, p, vsr_out_w, vsr_out_h, bits)) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
    }
    out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t*)p->out_buf, (int)p->out_buf_size,
                                     free_cuda_buf, p->cuda_ref, 0);
    if (!out->bufs[0]) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        talloc_free(out); mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
    }
    mp_image_setfmt(out, IMGFMT_CUDA);
    mp_image_sethwfmt(out, IMGFMT_CUDA, vfx_imgfmt_from_bits(bits));
    mp_image_params_guess_csp(&out->params);
    out->params.color = mpi->params.color; // preserve HDR metadata
    out->w = vsr_out_w; out->h = vsr_out_h;
    out->params.w = vsr_out_w; out->params.h = vsr_out_h;
    out->planes[0] = (uint8_t*)p->out_buf;
    out->stride[0] = vsr_out_pitch;
    out->num_planes = 1;
    out->hwctx = av_buffer_ref(p->av_hw_frames);
    p->out_buf = 0; p->out_buf_size = 0;

    // 保留像素宽高比（aspect override：解码器层 video-aspect-override
    // 经 mp_image_params_set_dsize 换算进 p_w/p_h——VSR 重建 params 时
    // 须带出，否则 VO 按方形像素渲染 → 画面比例设置失效。实测缺失时
    // aspect 选项点击无效果。PAR 与尺寸无关，放大后直接复制）
    out->params.p_w = mpi->params.p_w;
    out->params.p_h = mpi->params.p_h;

    out->pts         = mpi->pts;
    out->dts         = mpi->dts;
    out->nominal_fps = mpi->nominal_fps;

    MP_DBG(f, "vsr: frame %d  %dx%d -> %dx%d  scale=%.2f\n",
           p->frame_count, video_w, video_h, vsr_out_w, vsr_out_h, p->effective_scale);

    talloc_free(mpi);
    p->status_cost_ms += vsr_now_ms() - t0;
    p->status_frames++;
    if (p->status_frames >= 30) {
        double el = (vsr_now_ms() - p->status_t0) / 1000.0;
        if (p->status_t0 == 0)
            el = 0;
        if (el > 0)
            // 实际生效倍率（effective_scale，auto 下动态变化）——OSD 的
            // "设定|实际"对：设定段显示 UI 设置，状态段显示当前实际
            mp_msg(f->log, MSGL_STATUS,
                   "vsr-status: mode=active scale=%.2f cost=%.1fms fps=%.0f\n",
                   p->effective_scale,
                   p->status_cost_ms / p->status_frames,
                   p->status_frames / el);
        p->status_t0 = vsr_now_ms();
        p->status_frames = 0;
        p->status_cost_ms = 0;
    }
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
    cuCtxPopCurrent(NULL);
}

// ── vsr_command — VSR-specific screenshot capability ───────────────────────
// Independent of mpv's own screenshot (which only captures post-filter
// frames). Command (via `vf-command vsr ...`):
//   dump-both <input_path>|<output_path> — one frame, both sides:
//     input  = frame before VSR processing (original)
//     output = frame after VSR processing (Nx)
// Both buffers belong to the same processed frame, so a single command
// guarantees input/output pair from the identical frame.

static bool vsr_dump_frame(struct mp_filter *f, struct priv *p,
                           const char *path, bool output)
{
    if (!p->cuda_ref || !p->cuda_ref->ctx || !path || !path[0])
        return false;
    struct vsr_context *c = &p->vsr;
    int w  = output ? c->out_w  : c->in_w;
    int h  = output ? c->out_h  : c->in_h;
    int pitch = output ? c->out_pitch : c->in_pitch;
    CUdeviceptr src = output ? (CUdeviceptr)c->out_pixels
                             : (CUdeviceptr)c->in_pixels;
    if (!src || w <= 0 || h <= 0 || pitch <= 0)
        return false;

    // 输出格式随引擎位深：10-bit 引擎的缓冲是打包 RGB10A2，按 RGBA8 解释
    // 会得到垃圾图（调试命令，格式必须与引擎一致）。
    struct mp_image *img = mp_image_alloc(vfx_imgfmt_from_bits(c->bits), w, h);
    if (!img)
        return false;

    cuCtxPushCurrent(p->cuda_ref->ctx);
    CUDA_MEMCPY2D cp = {0};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice     = src;
    cp.srcPitch      = (size_t)pitch;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST;
    cp.dstHost       = img->planes[0];
    cp.dstPitch      = (size_t)img->stride[0];
    cp.WidthInBytes  = (size_t)w * 4;
    cp.Height        = (size_t)h;
    CUresult cr = cuMemcpy2D(&cp);
    cuCtxPopCurrent(NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "vsr: dump %s cuMemcpy2D failed (%d)\n",
               output ? "output" : "input", cr);
        talloc_free(img);
        return false;
    }

    dump_png(img, path, f->log);
    talloc_free(img);
    MP_VERBOSE(f, "vsr: dumped %s %dx%d -> %s\n",
               output ? "output" : "input", w, h, path);
    return true;
}

// ── 参数解析（与 filter 选项 OPT_CHOICE 字符串语义一致）──────────────
// vf-command 参数接受选项字符串（off/auto/2/3/4）或数字。
// 不能用 atoi 直接解析："off" → atoi=0（auto！）——UI 关闭 scale 发
// "off" 会被误判为 auto，输出分辨率不回退。

static bool parse_scale_arg(const char *arg, float *out)
{
    if (!strcmp(arg, "off")) { *out = -1; return true; }
    if (!strcmp(arg, "auto")) { *out = 0; return true; }
    // 非整数倍（SDK 支持 4/3、1.5，vfx_test6 实测）：分数写法在此
    // 换算（mpv OPT_FLOAT 仅 strtod，"4/3" 会误读为 4）。
    if (!strcmp(arg, "4/3")) { *out = 4.0f / 3.0f; return true; }
    if (!strcmp(arg, "1.5") || !strcmp(arg, "3/2")) { *out = 1.5f; return true; }
    char *end = NULL;
    float v = strtof(arg, &end);
    if (end == arg || *end != '\0') return false;
    // SDK 倍率集合校验：4/3, 1.5, 2, 3, 4（容差 0.01）——非法倍率
    // 会致 Load 失败（"Suitable ratio not found"），先拒后防。
    const float supported[] = {4.0f / 3.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        if (fabsf(v - supported[i]) < 0.01f) { *out = v; return true; }
    return false;
}

static bool parse_denoise_arg(const char *arg, int *out)
{
    if (!strcmp(arg, "off"))           *out = -1;
    else if (!strcmp(arg, "low"))      *out = 8;
    else if (!strcmp(arg, "medium"))   *out = 9;
    else if (!strcmp(arg, "high"))     *out = 10;
    else if (!strcmp(arg, "ultra"))    *out = 11;
    else *out = atoi(arg);
    return *out == -1 || (*out >= 8 && *out <= 11);
}

static bool parse_quality_arg(const char *arg, int *out)
{
    if (!strcmp(arg, "low"))           *out = 1;
    else if (!strcmp(arg, "medium"))   *out = 2;
    else if (!strcmp(arg, "high"))     *out = 3;
    else if (!strcmp(arg, "ultra"))    *out = 4;
    else *out = atoi(arg);
    return *out >= 1 && *out <= 4;
}

static bool vsr_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT)
        return false;
    // ── Runtime parameter update (via `vf-command @vsr <param> <value>`) ──
    // 热更新，不重建 filter 链。f_process 下一帧自动处理重配：
    //   scale   → resolve_scale 新值 → effective_scale 变化 → ensure_vsr
    //             轻量更新（SetImage 换输出缓冲，引擎常驻）
    //   quality → ensure_vsr 检测 quality 变化 → vsr_set_quality（SetU32）
    //   denoise → passthrough 判定（f_process）
    if (cmd->arg) {
        int v;
        float fv;
        if (strcmp(cmd->cmd, "scale") == 0) {
            if (!parse_scale_arg(cmd->arg, &fv))
                return false;   // 非法值（1 不允许——scale=1 即降噪模式）
            if (fabsf(p->opts->scale - fv) > 0.001f) {
                MP_INFO(f, "vsr: scale %.2f -> %.2f (vf-command)\n",
                        p->opts->scale, fv);
                p->opts->scale = fv;
            }
            return true;
        }
        if (strcmp(cmd->cmd, "seek-target") == 0) {
            // 客户端绝对 seek 前调用：pts < 目标的帧直接丢弃（0 = 清除）。
            char *end = NULL;
            double t = strtod(cmd->arg, &end);
            if (end == cmd->arg || *end != '\0' || !isfinite(t) || t < 0)
                return false;
            p->skip_before_pts = t;
            p->skip_count = 0;
            p->skip_passed = false;
            p->skip_expire_ms = vsr_now_ms() + 3000.0;
            MP_VERBOSE(f, "vsr: seek-target %.3f\n", p->skip_before_pts);
            return true;
        }
        if (strcmp(cmd->cmd, "quality") == 0) {
            if (!parse_quality_arg(cmd->arg, &v))
                return false;
            if (p->opts->quality != v) {
                MP_INFO(f, "vsr: quality %d -> %d (vf-command)\n",
                        p->opts->quality, v);
                p->opts->quality = v;
            }
            return true;
        }
        if (strcmp(cmd->cmd, "denoise") == 0) {
            if (!parse_denoise_arg(cmd->arg, &v))
                return false;
            if (p->opts->denoise != v) {
                MP_INFO(f, "vsr: denoise %d -> %d (vf-command)\n",
                        p->opts->denoise, v);
                p->opts->denoise = v;
            }
            return true;
        }
    }
    if (strcmp(cmd->cmd, "dump-both") == 0 && cmd->arg) {
        // arg: "input_path|output_path"
        char *sep = strchr((char *)cmd->arg, '|');
        if (!sep)
            return false;
        *sep = '\0';
        bool ok_in  = vsr_dump_frame(f, p, cmd->arg, false);
        bool ok_out = vsr_dump_frame(f, p, sep + 1, true);
        return ok_in && ok_out;
    }
    return false;
}

// ── f_reset ────────────────────────────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;

    // 只清每次 seek 的簿记，**引擎保留**（与 vf_vfg / vf_rife 的 f_reset 同款）：
    // mpv 每次 seek 都会 reset_playback_state → mp_filter_reset → 本回调，而
    // VFX 引擎 destroy + 重新 Load 是 ~300 ms 的固定成本（实测 1080p 精确 seek：
    // 无滤镜 35 ms、仅 VSR 330 ms，且与"跳多远"无关）——按住方向键逐帧步进时
    // 每一步都要白付一次。VSR 是逐帧空间操作、没有跨帧状态，跨 seek 复用安全：
    // 位深/输入尺寸变化仍由 ensure_vsr 的检查触发重建，CUDA context 变化仍由
    // ensure_cuda 处理（那条路径会主动 vsr_destroy）。
    p->frame_count = 0;
    p->status_emit = 0;
    MP_INFO(f, "vsr: reset (engine kept)\n");
}

// ── f_destroy ──────────────────────────────────────────────────────────────

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    bool ctx_pushed = false;
    if (p->cuda_ref && p->cuda_ref->ctx) {
        CUresult cr = cuCtxPushCurrent(p->cuda_ref->ctx);
        ctx_pushed = (cr == CUDA_SUCCESS);
    }

    vsr_destroy(&p->vsr);

    if (p->yuv_conv_ready) {
        yuv_to_rgba_destroy(&p->yuv_conv);
        p->yuv_conv_ready = false;
    }

    if (p->out_buf) {
        if (ctx_pushed) cuMemFree(p->out_buf);
        p->out_buf      = 0;
        p->out_buf_size = 0;
    }

    if (p->cuda_init_done) {
        if (ctx_pushed) cuStreamDestroy(p->cuda_stream);
        p->cuda_stream = NULL;
    }

    if (ctx_pushed)
        cuCtxPopCurrent(NULL);

    // frames/device 载体释放必须发生在无 push 状态：其 uninit 可能
    // cuCtxDestroy（FFmpeg 自建 ctx / hwdec 载体）。current_ctx 模式下
    // 借的 ctx 不在此销毁——由共享结构 refs 管理。
    if (p->av_hw_frames) {
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames = NULL;
    }
    if (p->av_hw_device) {
        av_buffer_unref(&p->av_hw_device);
        p->av_hw_device = NULL;
    }

    // Release the filter's context reference. Frames still in flight
    // (pin teardown happens after destroy()) hold their own references;
    // the context is destroyed only when the last one is released.
    if (p->cuda_ref) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
    }

    p->cuda_init_done = false;
}

// ── vf_vsr_filter_info ─────────────────────────────────────────────────────

static const struct mp_filter_info vf_vsr_filter = {
    .name      = "vsr",
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = vsr_command,
};

// ── vf_vsr_create ──────────────────────────────────────────────────────────

static struct mp_filter *vf_vsr_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_vsr_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);

    // 启动即知无需 VSR：scale∈{-1,1} 且 denoise=off。denoise>0 时
    // 不得 passthrough（1:1 降噪模式也要跑 VSR）。运行时参数变化
    // 由 f_process 逐帧重新评估（无 Static 分支——缓存会让 vf-command
    // 热更新失效，如关闭 scale 后点 2x/开降噪永远不生效）。
    p->passthrough = ((p->opts->scale == -1 || fabsf(p->opts->scale - 1.0f) < 0.001f)
                      && p->opts->denoise == -1);
    p->warmup_done = false;
    p->effective_scale = 1;

    if (p->passthrough) {
        MP_VERBOSE(f, "vsr: passthrough mode (scale=%.2f, denoise=%d)\n",
                   p->opts->scale, p->opts->denoise);
        return f;
    }

    return f;
}

// ── Registration ───────────────────────────────────────────────────────────

const struct mp_user_filter_entry vf_vsr = {
    .desc = {
        .name          = "vsr",
        .description   = "AI super-resolution via NVIDIA VFX SDK",
        .priv_size     = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_vsr_opts_def,
        .options       = vf_opts_fields,
    },
    .create = vf_vsr_create,
};
