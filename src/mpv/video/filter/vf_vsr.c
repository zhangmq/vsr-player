/*
 * vf_vsr.c -- Video Super Resolution filter via NVIDIA VFX SDK
 *
 * Manual format normalization:
 * SW (any YUV): mp_sws→RGBA → cuMemcpyHtoD → CUDA staging
 * HW CUDA/RGBA: passthrough
 * HW CUDA/YUV: yuv_to_rgba D2D
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

/// CUDA context with reference counting. Output frames hold a reference
/// (av_buffer opaque); the context is destroyed only when the last
/// reference is released. This decouples the context lifetime from the
/// filter's: frames can outlive the filter (pin teardown frees frames
/// AFTER f_destroy ran — use-after-free otherwise, cuCtxPopCurrent SEGV).
struct vsr_cuda_ref {
    CUcontext ctx;
    atomic_int refs;  // 1 = filter, +1 per output frame in flight
};

static void vsr_cuda_ref_unref(struct vsr_cuda_ref *r)
{
    if (atomic_fetch_sub(&r->refs, 1) != 1)
        return;
    cuCtxDestroy(r->ctx);
    free(r);
}

struct priv {
    struct vf_vsr_opts *opts;

    struct vsr_cuda_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    AVBufferRef *av_hw_device;   // shared CUDA device ctx
    AVBufferRef *av_hw_frames;   // CUDA/RGBA frames ctx for output

    struct vsr_context vsr;
    bool      vsr_configured;

    // yuv→rgba D2D
    struct yuv_to_rgba    yuv_conv;
    bool                  yuv_conv_ready;
    int                   yuv_conv_bd;
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

static bool ensure_cuda(struct mp_filter *f, struct priv *p)
{
    if (p->cuda_init_done) return true;

    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuInit failed (%d)\n", cr);
        return false;
    }

    CUdevice dev;
    cr = cuDeviceGet(&dev, 0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuDeviceGet(0) failed (%d)\n", cr);
        return false;
    }
    CUcontext cuda_ctx;
    cr = cuCtxCreate(&cuda_ctx, CU_CTX_SCHED_AUTO, dev, 0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuCtxCreate failed (%d)\n", cr);
        return false;
    }
    MP_VERBOSE(f, "vsr: created CUDA context on device 0\n");

    p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
    p->cuda_ref->ctx = cuda_ctx;
    atomic_init(&p->cuda_ref->refs, 1);  // filter holds one reference

    cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_NON_BLOCKING);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "cuStreamCreate failed (%d)\n", cr);
        return false;
    }

    p->cuda_init_done = true;
    return true;
}

// ── AVHWFramesContext for SW-path CUDA/RGBA output ─────────────────────────

static bool ensure_av_hw_frames(struct mp_filter *f, struct priv *p,
                                 int w, int h)
{
    // Recreate on dimension change — downstream autoconvert needs matching dims
    if (p->av_hw_frames) {
        AVHWFramesContext *fctx = (AVHWFramesContext *)p->av_hw_frames->data;
        if (fctx->width == w && fctx->height == h)
            return true;
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames = NULL;
    }

    if (!p->cuda_init_done) return false;

    AVBufferRef *dev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (!dev) {
        MP_ERR(f, "av_hwdevice_ctx_alloc failed\n");
        return false;
    }
    AVCUDADeviceContext *cuda_dev = (AVCUDADeviceContext *)
        ((AVHWDeviceContext *)dev->data)->hwctx;
    cuda_dev->cuda_ctx = p->cuda_ref->ctx;

    if (av_hwdevice_ctx_init(dev) < 0) {
        MP_ERR(f, "av_hwdevice_ctx_init failed\n");
        av_buffer_unref(&dev);
        return false;
    }
    p->av_hw_device = dev;

    AVBufferRef *frames = av_hwframe_ctx_alloc(dev);
    if (!frames) {
        MP_ERR(f, "av_hwframe_ctx_alloc failed\n");
        return false;
    }
    AVHWFramesContext *fctx = (AVHWFramesContext *)frames->data;
    fctx->format    = AV_PIX_FMT_CUDA;
    fctx->sw_format = AV_PIX_FMT_RGBA;
    fctx->width     = w;
    fctx->height    = h;

    if (av_hwframe_ctx_init(frames) < 0) {
        MP_ERR(f, "av_hwframe_ctx_init(frames) failed\n");
        av_buffer_unref(&frames);
        return false;
    }
    p->av_hw_frames = frames;

    MP_VERBOSE(f, "vsr: created CUDA/RGBA hw_frames_ctx %dx%d\n", w, h);
    return true;
}

// ── VSR init / reconfigure ─────────────────────────────────────────────────

static bool ensure_vsr(struct mp_filter *f, struct priv *p,
                        int in_w, int in_h, int out_w, int out_h,
                        int quality)
{
    if (p->vsr_configured) {
        // ── 引擎存在：尺寸不变 → quality 轻量更新；尺寸变化 → 重建 ──
        // SDK 实验结论（vfx_test5）：SetImage 换尺寸输出错乱（PSNR
        // 7.4dB，引擎内部 tile 切分与 Load 时尺寸绑定）→ 输入/输出
        // 尺寸变化必须重建管线；QualityLevel 可 Load 后 SetU32 更新
        //（q1/q3 输出结构化差异 44.8dB）→ quality 轻量。
        if (p->vsr.in_w == in_w && p->vsr.in_h == in_h
            && p->vsr.out_w == out_w && p->vsr.out_h == out_h) {
            if (p->vsr.quality != quality) {
                if (!vsr_set_quality(&p->vsr, quality))
                    return false;
            }
            return true;
        }
        // 尺寸变化：销毁后走 init 重建（vsr_destroy 内含 stream 同步，
        // 无销毁竞态；ensure_img 按需分配，无泄漏）
        vsr_destroy(&p->vsr);
        p->vsr_configured = false;
    }

    // 引擎不存在（filter 新建 / 尺寸变化重建 / passthrough 回切）：init
    MP_VERBOSE(f, "vsr: init %dx%d -> %dx%d quality=%d\n",
               in_w, in_h, out_w, out_h, quality);
    if (!vsr_init(&p->vsr, in_w, in_h, out_w, out_h, quality,
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
    struct vsr_cuda_ref *r = opaque;
    if (r && r->ctx) cuCtxPushCurrent(r->ctx);
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        vsr_cuda_ref_unref(r);
}

// ── yuv→rgba converter lifecycle ───────────────────────────────────────────

static bool ensure_yuv_converter(struct mp_filter *f, struct priv *p,
                                  int bit_depth, enum yuv_matrix matrix,
                                  enum yuv_range range)
{
    if (p->yuv_conv_ready &&
        p->yuv_conv_bd == bit_depth &&
        p->yuv_conv_matrix == matrix &&
        p->yuv_conv_range == range)
        return true;

    if (p->yuv_conv_ready)
        yuv_to_rgba_destroy(&p->yuv_conv);

    if (!yuv_to_rgba_init(&p->yuv_conv, bit_depth, matrix, range)) {
        MP_ERR(f, "yuv_to_rgba_init failed (bd=%d)\n", bit_depth);
        p->yuv_conv_ready = false;
        return false;
    }

    p->yuv_conv_bd = bit_depth;
    p->yuv_conv_matrix = matrix;
    p->yuv_conv_range = range;
    p->yuv_conv_ready = true;
    return true;
}

// ── Color matrix helpers ───────────────────────────────────────────────────

static enum yuv_matrix matrix_from_repr(struct mp_image_params *p)
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

static enum yuv_range range_from_levels(enum pl_color_levels levels)
{
    return (levels == PL_COLOR_LEVELS_FULL) ? YUV_RANGE_FULL : YUV_RANGE_LIMITED;
}

// ── f_process ──────────────────────────────────────────────────────────────

#ifdef VSR_DEBUG
static void dump_gpu(const char *label, CUdeviceptr src, int pitch, int w, int h) {
    system("mkdir -p logs/dumps");
    size_t row = (size_t)w * 4, sz = row * h;
    void *buf = malloc(sz);
    if (!buf) return;
    CUDA_MEMCPY2D d = {0};
    d.srcMemoryType = CU_MEMORYTYPE_DEVICE; d.srcDevice = src;
    d.srcPitch = (size_t)pitch;
    d.dstMemoryType = CU_MEMORYTYPE_HOST; d.dstHost = buf;
    d.dstPitch = row; d.WidthInBytes = row; d.Height = (size_t)h;
    cuMemcpy2D(&d);
    char path[256];
    snprintf(path, sizeof(path), "logs/dumps/%s_%dx%d.rgba", label, w, h);
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(buf, 1, sz, fp); fclose(fp); }
    free(buf);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ffmpeg -y -f rawvideo -pixel_format rgba -video_size %dx%d -i %s -frames:v 1 logs/dumps/%s_%dx%d.png 2>/dev/null",
             w, h, path, label, w, h);
    system(cmd);
    fprintf(stderr, "VSR_DUMP: %s %dx%d pitch=%d\n", label, w, h, pitch);
}
static void dump_host(const char *label, const void *src, int stride, int w, int h) {
    system("mkdir -p logs/dumps");
    size_t row = (size_t)w * 4;
    char path[256];
    snprintf(path, sizeof(path), "logs/dumps/%s_%dx%d.rgba", label, w, h);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        for (int y = 0; y < h; y++)
            fwrite((const uint8_t*)src + y * stride, 1, row, fp);
        fclose(fp);
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ffmpeg -y -f rawvideo -pixel_format rgba -video_size %dx%d -i %s -frames:v 1 logs/dumps/%s_%dx%d.png 2>/dev/null",
             w, h, path, label, w, h);
    system(cmd);
    fprintf(stderr, "VSR_DUMP: %s %dx%d stride=%d\n", label, w, h, stride);
}
#endif

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
        MP_ERR(f, "vsr: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *mpi = frame.data;
    p->frame_count++;

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
    p->passthrough = (fabsf(p->effective_scale - 1.0f) < 0.001f && p->opts->denoise == -1);
    if (p->passthrough) {
        if (p->vsr_configured) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            vsr_destroy(&p->vsr);
            cuCtxPopCurrent(NULL);
            p->vsr_configured = false;
            MP_INFO(f, "vsr: switching to passthrough (scale=%.2f, denoise=off)\n",
                    p->effective_scale);
        }
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    if (!ensure_cuda(f, p)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    cuCtxPushCurrent(p->cuda_ref->ctx);

    if (!ensure_vsr(f, p, video_w, video_h, out_w, out_h, p->opts->quality)) {
        // 首次失败：销毁引擎状态重试一次（瞬时失败自愈，如 GPU 忙）。
        if (p->vsr_configured) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            vsr_destroy(&p->vsr);
            cuCtxPopCurrent(NULL);
            p->vsr_configured = false;
        }
        if (!ensure_vsr(f, p, video_w, video_h, out_w, out_h,
                        p->opts->quality)) {
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

    if (is_hw && mpi->params.hw_subfmt != IMGFMT_RGBA) {
        // HW YUV → D2D to vsr.in_pixels
        int subfmt = mpi->params.hw_subfmt;
        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(subfmt);
        int bd = desc.bpp[0];
        enum yuv_matrix mx = matrix_from_repr(&mpi->params);
        enum yuv_range  rn = range_from_levels(mpi->params.repr.levels);
        if (!ensure_yuv_converter(f, p, bd, mx, rn)) {
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
    } else if (!is_hw) {
        // SW: swscale→RGBA → upload directly to vsr.in_pixels
        struct mp_image *rgba = mpi;
        if (mpi->imgfmt != IMGFMT_RGBA) {
            struct mp_sws_context *sws = mp_sws_alloc(f);
            if (!sws) {
                mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
            }
            sws->force_scaler = MP_SWS_AUTO;
            rgba = mp_image_alloc(IMGFMT_RGBA, video_w, video_h);
            if (!rgba) {
                talloc_free(sws); mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
            }
            mp_image_copy_attributes(rgba, mpi);
            if (mp_sws_scale(sws, rgba, mpi) < 0) {
                MP_ERR(f, "vsr: sws_scale failed\n");
                talloc_free(sws); talloc_free(rgba); mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
            }
            talloc_free(sws);
        }
        {
            CUDA_MEMCPY2D copy = {0};
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost       = rgba->planes[0];
            copy.srcPitch      = (size_t)rgba->stride[0];
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice     = (CUdeviceptr)p->vsr.in_pixels;
            copy.dstPitch      = (size_t)p->vsr.in_pitch;
            copy.WidthInBytes  = (size_t)video_w * 4;
            copy.Height        = (size_t)video_h;
            cuMemcpy2DAsync(&copy, p->cuda_stream);
        }
        if (rgba != mpi) {
#ifdef VSR_DEBUG
            if (p->frame_count == 1) dump_host("00_mpi_sws_rgba", rgba->planes[0], rgba->stride[0], video_w, video_h);
#endif
            talloc_free(rgba);
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

    // ── DEBUG: per-node dumps (frame 1 only) ─────────────────────────────
#ifdef VSR_DEBUG
    if (p->frame_count == 1) {
        dump_gpu("01_vsr_input", (CUdeviceptr)p->vsr.in_pixels, p->vsr.in_pitch, video_w, video_h);
        dump_gpu("02_vsr_output", p->out_buf, vsr_out_pitch, vsr_out_w, vsr_out_h);
    }
#endif

    // ── Output ───────────────────────────────────────────────────────────
    // Each output frame takes a reference on the CUDA context: the frame
    // can outlive the filter (pin teardown frees frames after f_destroy),
    // so free_cuda_buf must never touch a destroyed context.
    atomic_fetch_add(&p->cuda_ref->refs, 1);
    struct mp_image *out;
    if (mpi->hwctx) {
        out = talloc_zero(NULL, struct mp_image);
        talloc_set_destructor(out, mp_image_dtor);
        out->bufs[0] = av_buffer_create((uint8_t*)p->out_buf, (int)p->out_buf_size,
                                         free_cuda_buf, p->cuda_ref, 0);
        if (!out->bufs[0]) {
            vsr_cuda_ref_unref(p->cuda_ref);
            talloc_free(out); mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
        mp_image_setfmt(out, IMGFMT_CUDA);
        mp_image_sethwfmt(out, IMGFMT_CUDA, IMGFMT_RGBA);
        mp_image_params_guess_csp(&out->params);
        out->params.color = mpi->params.color; // preserve HDR metadata
        out->w = vsr_out_w; out->h = vsr_out_h;
        out->params.w = vsr_out_w; out->params.h = vsr_out_h;
        out->planes[0] = (uint8_t*)p->out_buf;
        out->stride[0] = vsr_out_pitch;
        out->num_planes = 1;
        out->hwctx = av_buffer_ref(mpi->hwctx);
        p->out_buf = 0; p->out_buf_size = 0;
    } else {
        if (!ensure_av_hw_frames(f, p, vsr_out_w, vsr_out_h)) {
            vsr_cuda_ref_unref(p->cuda_ref);
            mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
        out = talloc_zero(NULL, struct mp_image);
        talloc_set_destructor(out, mp_image_dtor);
        out->bufs[0] = av_buffer_create((uint8_t*)p->out_buf, (int)p->out_buf_size,
                                         free_cuda_buf, p->cuda_ref, 0);
        if (!out->bufs[0]) {
            vsr_cuda_ref_unref(p->cuda_ref);
            talloc_free(out); mp_frame_unref(&frame); mp_filter_internal_mark_failed(f); cuCtxPopCurrent(NULL); return;
        }
        mp_image_setfmt(out, IMGFMT_CUDA);
        mp_image_sethwfmt(out, IMGFMT_CUDA, IMGFMT_RGBA);
        mp_image_params_guess_csp(&out->params);
        out->params.color = mpi->params.color; // preserve HDR metadata
        out->w = vsr_out_w; out->h = vsr_out_h;
        out->params.w = vsr_out_w; out->params.h = vsr_out_h;
        out->planes[0] = (uint8_t*)p->out_buf;
        out->stride[0] = vsr_out_pitch;
        out->num_planes = 1;
        out->hwctx = av_buffer_ref(p->av_hw_frames);
        p->out_buf = 0; p->out_buf_size = 0;
    }

    out->pts         = mpi->pts;
    out->dts         = mpi->dts;
    out->nominal_fps = mpi->nominal_fps;

    MP_DBG(f, "vsr: frame %d  %dx%d -> %dx%d  scale=%.2f\n",
           p->frame_count, video_w, video_h, vsr_out_w, vsr_out_h, p->effective_scale);

    talloc_free(mpi);
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

    struct mp_image *img = mp_image_alloc(IMGFMT_RGBA, w, h);
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

    if (p->cuda_ref && p->cuda_ref->ctx) cuCtxPushCurrent(p->cuda_ref->ctx);
    vsr_destroy(&p->vsr);
    if (p->cuda_ref && p->cuda_ref->ctx) cuCtxPopCurrent(NULL);
    p->vsr_configured = false;
    p->frame_count = 0;
    MP_VERBOSE(f, "vsr: reset\n");
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

    if (p->av_hw_frames) {
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames = NULL;
    }
    if (p->av_hw_device) {
        av_buffer_unref(&p->av_hw_device);
        p->av_hw_device = NULL;
    }

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

    // Release the filter's context reference. Frames still in flight
    // (pin teardown happens after destroy()) hold their own references;
    // the context is destroyed only when the last one is released.
    if (p->cuda_ref) {
        vsr_cuda_ref_unref(p->cuda_ref);
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
