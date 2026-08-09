// vf_rife.c — RIFE AI frame interpolation (TensorRT) as an mpv filter.
//
// Pipeline: decode → [vf_rife] → [vf_vsr] → VO. Runs BEFORE super-resolution
// (interpolates at source resolution). Input: IMGFMT_CUDA frames (NV12/P010);
// SW frames pass through untouched.
//
// Scheduling: a target output frame rate (UI: off/30/40/60, CLI: [1,120],
// auto = 2×src) defines an absolute-time output grid. Each source-frame pair
// (t_prev, t_cur) produces all grid points in (t_prev, t_cur]; grid points that
// coincide with the source frame emit it as-is, others are interpolated with
// t = (t_grid - t_prev)/(t_cur - t_prev). Fractional factors (24→60 = 2.5)
// fall out naturally.
//
// Adaptive passthrough (normal mode): if measured per-pair interpolation cost
// exceeds the per-frame budget (1/src_fps − VSR estimate), degrade to
// passthrough once (MP_WARN), until reset/option change. Benchmark mode
// (scale > 0) never passes through — it measures the device ceiling.
//
// The engine (rife_lite_fp16_{PH}x{PW}.engine, fixed full-frame FP16 shape
// padded to 128 multiples) is loaded once and kept across seeks — TRT
// execution contexts are stateless between enqueues.
//
// Scene changes (frame-pair MAE > RIFE_SC_MAE) pass the previous frame
// through instead of interpolating — vs-mlrt SceneChangeNext convention
// (interpolating across a cut produces ghosting; the offline heya full-run
// found 276 such pairs in 13439).

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "common/msg.h"
#include "common/common.h"
#include "options/m_option.h"
#include "video/mp_image.h"
#include "mpv/client.h"   // struct mpv_node（自定义 option type 的 set/get）

#include "rife_internal.h"
#include "yuv_to_rgba.h"
#include "cuda_shared.h"

// ── Options ────────────────────────────────────────────────────────────

struct vf_rife_opts {
    int  fps;       // -1=off, 0=auto (2×src), [1,120] target fps
    int  scale;     // benchmark multiplier: 0=off, 2/3/4 (overrides fps)
    bool adaptive;  // normal-mode cost-based passthrough
};

#define OPT_BASE_STRUCT struct vf_rife_opts

// fps custom type: "off"→-1, "auto"→0, integer [1,120] (mirrors vf_vsr scale type)
static int rife_fps_parse(struct mp_log *log, const m_option_t *opt,
                          struct bstr name, struct bstr param, void *dst)
{
    int v;
    if (bstr_equals0(param, "off")) {
        v = -1;
    } else if (bstr_equals0(param, "auto")) {
        v = 0;
    } else {
        if (param.len == 0)
            return M_OPT_MISSING_PARAM;
        struct bstr rest;
        long long n = bstrtoll(param, &rest, 10);
        if (rest.len || n < 1 || n > 120) {
            mp_err(log, "The %.*s option must be off, auto, or an integer "
                   "1..120: %.*s\n", BSTR_P(name), BSTR_P(param));
            return M_OPT_INVALID;
        }
        v = (int)n;
    }
    if (dst)
        *(int *)dst = v;
    return 1;
}

static char *rife_fps_print(const m_option_t *opt, const void *val)
{
    return talloc_asprintf(NULL, "%d", *(const int *)val);
}

static void rife_fps_copy(const m_option_t *opt, void *dst, const void *src)
{
    if (dst && src)
        memcpy(dst, src, sizeof(int));
}

static int rife_fps_set(const m_option_t *opt, void *dst, struct mpv_node *src)
{
    int v;
    if (src->format == MPV_FORMAT_INT64)
        v = (int)src->u.int64;
    else if (src->format == MPV_FORMAT_DOUBLE)
        v = (int)src->u.double_;
    else
        return M_OPT_INVALID;
    if (v < -1 || v > 120 || v == 0)
        return M_OPT_OUT_OF_RANGE;
    *(int *)dst = v;
    return 1;
}

static int rife_fps_get(const m_option_t *opt, void *ta_parent,
                        struct mpv_node *dst, void *src)
{
    dst->format = MPV_FORMAT_INT64;
    dst->u.int64 = *(const int *)src;
    return 1;
}

static bool rife_fps_equal(const m_option_t *opt, void *a, void *b)
{
    return memcmp(a, b, sizeof(int)) == 0;
}

static const m_option_type_t rife_fps_type = {
    .name  = "rife-fps",
    .size  = sizeof(int),
    .parse = rife_fps_parse,
    .print = rife_fps_print,
    .copy  = rife_fps_copy,
    .set   = rife_fps_set,
    .get   = rife_fps_get,
    .equal = rife_fps_equal,
};

#define OPT_RIFE_FPS(field) \
    OPT_TYPED_FIELD(rife_fps_type, int, field)

static const struct m_option vf_opts_fields[] = {
    {"fps",      OPT_RIFE_FPS(fps)},
    {"scale",    OPT_CHOICE(scale, {"off", 0}, {"2", 2}, {"3", 3}, {"4", 4}),
                 M_RANGE(0, 4)},
    {"adaptive", OPT_BOOL(adaptive)},
    {0}
};

static const struct vf_rife_opts vf_rife_opts_def = {
    .fps = -1, .scale = 0, .adaptive = true,
};

// ── priv ───────────────────────────────────────────────────────────────

#define RIFE_ADAPT_WIN     10
#define RIFE_ADAPT_WARMUP  30     // pairs measured before decision
#define RIFE_ADAPT_SKIP    4      // first pairs (cold start) not recorded
#define RIFE_MAX_PER_PAIR  64     // pathological small-delta cap
#define RIFE_BUDGET_VSR_MS 12.0   // downstream VSR cost estimate for budget
#define RIFE_SC_MAE        20.0   // scene-change threshold (mean abs diff,
                                  // 0-255 scale — matches stats_repeat.py /
                                  // run_rife_trt_video.py offline runs)

enum rife_mode {
    RIFE_PASSTHROUGH,
    RIFE_ACTIVE,
};

struct rife_cuda_ref {
    CUcontext ctx;
    atomic_int refs;   // 1 = filter, +1 per in-flight output frame
};

struct priv {
    struct vf_rife_opts *opts;

    struct rife_cuda_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    struct yuv_to_rgba yuv_conv;
    bool      yuv_conv_ready;
    int       yuv_bd;
    enum yuv_matrix yuv_matrix;
    enum yuv_range  yuv_range;

    AVBufferRef *device_ref;   // frames-ctx ref keeping the hwdec ctx alive

    struct rife_context eng;
    bool  engine_ok;          // engine + kernels ready
    bool  degrade_warned;
    bool  sw_warned;          // SW-input passthrough warned

    // scheduling / grid
    int    video_w, video_h;  // engine's current size (set at init/reconfig)
    int    in_w, in_h;        // last seen input size (fires the size block
                              // once per real change — in passthrough the
                              // engine never inits, so video_w/h stay 0)
    double src_fps;           // nominal_fps or measured
    double out_fps;           // output grid spacing
    double frame_budget_ms;   // adaptive budget: 1/src_fps − VSR estimate
    double t0;                // pts of first frame (grid origin)
    bool   have_prev;
    double prev_pts;          // pair left endpoint (pts only — the frame
                              // itself is released right after the pair:
                              // its RGBA lives in staging, and holding the
                              // hwdec buffer between pairs presses the
                              // nvdec surface pool — decoding stalls once
                              // every surface is held (observed stall).
    struct mp_image *prev;    // owned source frame — held only for the EOF
                              // flush; released when the next pair starts
    bool   cur_emitted;       // last frame's grid point already output?
    bool   eof_seen;

    // output queue (one output per process() call)
    struct mp_image *queue[RIFE_MAX_PER_PAIR + 2];
    int queue_len, queue_pos;

    enum rife_mode mode;
    char  pt_reason[32];      // passthrough reason (for the status line)

    // adaptive measurement
    bool    adapt_warmup_done;
    double  pair_cost_ms[RIFE_ADAPT_WIN];
    int     pair_count, adapt_idx;

    int frame_count;
};

// ── Status line (MSGL_STATUS, ~2Hz) ──────────────────────────────────
// Consumed by the client: the event thread picks up "fruc-status:" lines
// and shows them in the OSD (debugging the interpolation live).

#define RIFE_STATUS_EVERY 30   // frames between status lines (~0.5s @60fps)

static double adapt_avg(struct priv *p);   // defined below (adaptive section)

static void rife_status(struct mp_filter *f, struct priv *p)
{
    char buf[256];
    int n = 0;
    if (p->mode == RIFE_ACTIVE) {
        n = snprintf(buf, sizeof(buf),
                     "fruc-status: mode=active src=%.2f out=%.2f pad=%dx%d",
                     p->src_fps, p->out_fps,
                     p->eng.ph, p->eng.pw);
        if (p->opts->adaptive && p->adapt_warmup_done)
            n += snprintf(buf + n, sizeof(buf) - n,
                          " cost=%.1fms budget=%.1fms",
                          adapt_avg(p), p->frame_budget_ms);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "fruc-status: mode=passthrough reason=%s src=%.2f out=%.2f",
                     p->pt_reason, p->src_fps, p->out_fps);
    }
    snprintf(buf + n, sizeof(buf) - n, " frames=%d", p->frame_count);
    mp_msg(f->log, MSGL_STATUS, "%s\n", buf);
}

// ── CUDA context (mirror vf_vsr.c) ────────────────────────────────────
// Borrows the hwdec's CUDA context from the frame hwctx (single-context
// design — see cuda_shared.h). The context is never destroyed here: its
// lifetime is governed by the frame hwctx refs and the filter's device ref.

static void rife_cuda_ref_unref(struct rife_cuda_ref *r)
{
    if (!r) return;
    if (atomic_fetch_sub(&r->refs, 1) == 1)
        free(r);   // ctx borrowed from hwdec — never destroy
}

static bool ensure_cuda(struct mp_filter *f, struct priv *p,
                        struct mp_image *cur)
{
    CUcontext borrow = mp_cuda_ctx_from_mpi(cur);
    if (!borrow) {
        MP_ERR(f, "rife: hardware frame has no CUDA context\n");
        return false;
    }
    if (p->cuda_ref && p->cuda_ref->ctx == borrow)
        return true;

    if (p->cuda_ref) {
        rife_cuda_ref_unref(p->cuda_ref);
        p->cuda_ref = NULL;
    }
    if (p->cuda_init_done) {
        cuStreamDestroy(p->cuda_stream);
        p->cuda_stream = NULL;
    }
    if (p->device_ref) {
        av_buffer_unref(&p->device_ref);
        p->device_ref = NULL;
    }

    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        MP_ERR(f, "rife: cuInit failed (%d)\n", res);
        return false;
    }
    if (!mp_cuda_ref_device(cur, &p->device_ref)) {
        MP_ERR(f, "rife: failed to hold hwdec device ref\n");
        return false;
    }

    p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
    p->cuda_ref->ctx = borrow;
    atomic_init(&p->cuda_ref->refs, 1);
    cuCtxPushCurrent(borrow);
    bool ok = cuStreamCreate(&p->cuda_stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS;
    cuCtxPopCurrent(NULL);
    if (!ok) {
        MP_ERR(f, "rife: cuStreamCreate failed\n");
        rife_cuda_ref_unref(p->cuda_ref);
        p->cuda_ref = NULL;
        return false;
    }
    p->cuda_init_done = true;
    return true;
}

// ── yuv_to_rgba converter (mirror vf_vsr.c) ───────────────────────────

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

static bool ensure_yuv_converter(struct mp_filter *f, struct priv *p,
                                 int bit_depth, enum yuv_matrix mx,
                                 enum yuv_range rn)
{
    if (p->yuv_conv_ready && p->yuv_bd == bit_depth &&
        p->yuv_matrix == mx && p->yuv_range == rn)
        return true;
    if (p->yuv_conv_ready) {
        yuv_to_rgba_destroy(&p->yuv_conv);
        p->yuv_conv_ready = false;
    }
    if (!yuv_to_rgba_init(&p->yuv_conv, bit_depth, mx, rn)) {
        MP_ERR(f, "rife: yuv_to_rgba_init failed\n");
        return false;
    }
    p->yuv_conv_ready = true;
    p->yuv_bd = bit_depth;
    p->yuv_matrix = mx;
    p->yuv_range = rn;
    return true;
}

// ── Output buffer (mirror vf_vsr.c:502-514, 880-930) ──────────────────

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
    struct rife_cuda_ref *r = opaque;
    if (r && r->ctx) cuCtxPushCurrent(r->ctx);
    // No stream sync — see vf_vsr.c free_cuda_buf for the reasoning: the
    // buffer is only freed after its consumers (vsr's copy on its stream,
    // or the VO's map copy under retained-frame semantics) are done, and a
    // stream-0 sync on the shared context would wait on the render thread.
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        rife_cuda_ref_unref(r);
}

static struct mp_image *rife_make_output(struct mp_filter *f, struct priv *p,
                                         CUdeviceptr buf, int w, int h, int pitch,
                                         const struct mp_image *src, double pts)
{
    atomic_fetch_add(&p->cuda_ref->refs, 1);
    struct mp_image *out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t *)buf, pitch * h,
                                    free_cuda_buf, p->cuda_ref, 0);
    mp_image_setfmt(out, IMGFMT_CUDA);
    mp_image_sethwfmt(out, IMGFMT_CUDA, IMGFMT_RGBA);
    mp_image_params_guess_csp(&out->params);
    out->params.color = src->params.color;
    out->params.p_w = src->params.p_w;
    out->params.p_h = src->params.p_h;
    out->w = w;
    out->h = h;
    out->params.w = w;
    out->params.h = h;
    out->planes[0] = (uint8_t *)buf;
    out->stride[0] = pitch;
    out->num_planes = 1;
    out->hwctx = av_buffer_ref(src->hwctx);
    out->pts = pts;
    out->dts = src->dts;
    out->nominal_fps = p->out_fps;
    out->pkt_duration = 1.0 / p->out_fps;
    return out;
}

// ── Adaptive measurement ───────────────────────────────────────────────

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void adapt_record(struct priv *p, double cost_ms)
{
    p->pair_count++;
    if (p->pair_count <= RIFE_ADAPT_SKIP)
        return;  // cold start (engine JIT etc)
    p->pair_cost_ms[p->adapt_idx] = cost_ms;
    p->adapt_idx = (p->adapt_idx + 1) % RIFE_ADAPT_WIN;
    if (p->pair_count - RIFE_ADAPT_SKIP >= RIFE_ADAPT_WIN)
        p->adapt_warmup_done = true;
}

static double adapt_avg(struct priv *p)
{
    int n = p->pair_count - RIFE_ADAPT_SKIP;
    if (n > RIFE_ADAPT_WIN) n = RIFE_ADAPT_WIN;
    if (n <= 0) return 0;
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += p->pair_cost_ms[i];
    return sum / n;
}

// ── Mode decision ─────────────────────────────────────────────────────

static bool decide_mode(struct mp_filter *f, struct priv *p,
                        struct mp_image *cur)
{
    bool benchmark = p->opts->scale > 0;

    if (!benchmark) {
        if (p->opts->fps == -1) {
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "off");
            return false;
        }
        if (cur->w > 1920 || cur->h > 1080) {
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "resolution");
            return false;
        }
        double src_fps = p->src_fps;
        if (p->opts->fps > 0 && src_fps > 0 && src_fps >= p->opts->fps) {
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "src-fps");
            return false;
        }
        if (p->opts->adaptive && p->adapt_warmup_done &&
            adapt_avg(p) > p->frame_budget_ms * 1.05) {
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "cost");
            if (!p->degrade_warned) {
                MP_WARN(f, "rife: adaptive passthrough — avg %.1fms > budget %.1fms\n",
                        adapt_avg(p), p->frame_budget_ms);
                p->degrade_warned = true;
            }
            return false;
        }
    }
    p->mode = RIFE_ACTIVE;
    return true;
}

// ── Pair processing ───────────────────────────────────────────────────

static bool process_pair(struct mp_filter *f, struct priv *p,
                         double t_prev, double t_cur,
                         struct mp_image *cur, bool *grid_emitted_cur)
{
    double t_start = now_ms();

    // convert cur NV12 → RGBA staging (rgba_b); prev already in rgba_a.
    // Reads the hwdec frame planes directly — same CUDA context now, no
    // staging copy. Decode is many frames ahead of filter processing, so
    // the decoder's stream-0 writes have long completed by the time this
    // kernel runs (the pipeline is serialized by per-filter stream syncs).
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(cur->params.hw_subfmt);
    int bd = desc.bpp[0];
    enum yuv_matrix mx = matrix_from_repr(&cur->params);
    enum yuv_range rn = range_from_levels(cur->params.repr.levels);
    if (!ensure_yuv_converter(f, p, bd, mx, rn))
        return false;
    // Reads the hwdec frame planes directly — same CUDA context, no staging
    // copy. nvdec's pool copies run on the legacy default stream, which
    // implicitly synchronizes with prior work on our stream; the reverse
    // ordering is de facto given by the decode being many frames ahead of
    // filter processing (same assumption vf_vsr has always made).
    if (!yuv_to_rgba_convert(&p->yuv_conv,
                             cur->planes[0], cur->stride[0],
                             cur->planes[1], cur->stride[1],
                             p->video_w, p->video_h,
                             (void *)p->eng.rgba_b, p->eng.rgba_pitch,
                             p->cuda_stream)) {
        MP_ERR(f, "rife: yuv_to_rgba_convert failed\n");
        return false;
    }

    // scene change detection (mean |prev-cur| on the staging pair; syncs the
    // stream for the host read — that sync doubles as this pair's early
    // boundary: the staging convert is complete before any emit below)
    bool scene = rife_scene_change(&p->eng, p->cuda_stream, RIFE_SC_MAE);
    if (scene)
        MP_DBG(f, "rife: scene change (MAE > %.0f) — pass-through\n", RIFE_SC_MAE);

    // ── Output scheduling ────────────────────────────────────────────
    // vs-mlrt semantics for integer factors >= 2 (the community RIFE
    // model): output = Interleave([src, interp]) — the source frame is
    // always retained and each interpolated frame sits at a RELATIVE
    // timepoint t = i/k between the pair (t = 0.5 for 2×). An absolute
    // grid (t0 + n/out_fps) instead makes tval swing between ~0.01 and
    // ~0.99 on VFR sources (e.g. goose 33.0/34.0ms alternating PTS),
    // which forces RIFE to interpolate at extreme times and swallows
    // the source frame (never on-grid) — observed as content stutter.
    // Non-integer factors (< 2× or 24→60=2.5) keep the absolute grid.
    double factor = p->out_fps / p->src_fps;
    long long k = llround(factor);
    bool integer_factor = k >= 2 && fabs(factor - k) < 0.01;

    *grid_emitted_cur = false;
    bool ok = true;

    if (integer_factor) {
        // [mid(1/k), mid(2/k), ..., cur] — source frame always emitted.
        // CONTENT time is the relative midpoint (tval = i/k, vs-mlrt
        // Interleave semantics — no extreme tval on VFR sources). PTS is
        // stamped on the absolute output grid (t0 + n/out_fps), matching
        // minterpolate's `out_pts++` counter / vf_vapoursynth's uniform
        // accumulation: a PTS sequence that follows the input (33.0/34.0ms
        // alternating on goose) makes display_sync accumulate ±0.3ms
        // vsync rounding errors and periodically repeat/drop frames —
        // the offline pipeline has no PTS, hence no stutter (verified).
        // Each pair emits k frames ending at cur's nearest grid point
        // (round, not floor — floor would re-emit the previous pair's
        // last grid point on short VFR intervals).
        long long n_cur = llround((t_cur - p->t0) * p->out_fps);
        for (long long i = 0; i < k; i++) {
            double tval = (double)(i + 1) / k;
            double tg = p->t0 + (n_cur - (k - 1) + i) / p->out_fps;
            if (i == k - 1) {
                struct mp_image *out = mp_image_new_ref(cur);
                if (out) {
                    out->pts = tg;   // grid PTS, not the raw input PTS
                    out->nominal_fps = p->out_fps;
                    out->pkt_duration = 1.0 / p->out_fps;
                }
                p->queue[p->queue_len++] = out;
                *grid_emitted_cur = true;
            } else {
                CUdeviceptr out_buf = 0;
                if (cuMemAlloc(&out_buf, (size_t)p->eng.rgba_pitch * p->video_h)
                        != CUDA_SUCCESS) {
                    MP_ERR(f, "rife: output alloc failed\n");
                    ok = false;
                    break;
                }
                bool ok_run = scene
                    // minterpolate scene-change semantics: duplicate the
                    // temporally nearer endpoint (alpha > 0.5 → next, else
                    // previous) — copying the far frame would show content
                    // from the wrong side of the cut.
                    ? rife_pass_through(&p->eng, p->cuda_stream, tval > 0.5,
                                        out_buf, p->eng.rgba_pitch)
                    : rife_interpolate(&p->eng, p->cuda_stream, tval,
                                       out_buf, p->eng.rgba_pitch);
                if (!ok_run) {
                    cuMemFree(out_buf);
                    MP_ERR(f, "rife: %s failed\n", scene ? "pass-through" : "interpolate");
                    ok = false;
                    break;
                }
                struct mp_image *out = rife_make_output(f, p, out_buf,
                                                        p->video_w, p->video_h,
                                                        p->eng.rgba_pitch,
                                                        cur, tg);
                p->queue[p->queue_len++] = out;
            }
        }
    } else {
        // absolute grid (non-integer factors)
        double fps = p->out_fps;
        long long n0 = (long long)floor((t_prev - p->t0) * fps) + 1;
        long long n1 = (long long)floor((t_cur - p->t0) * fps);
        if (n1 - n0 + 1 > RIFE_MAX_PER_PAIR)
            n1 = n0 + RIFE_MAX_PER_PAIR - 1;
        MP_DBG(f, "rife: pair (%.3f, %.3f) grid %lld..%lld%s\n", t_prev, t_cur, n0, n1,
               scene ? " [scene]" : "");
        for (long long n = n0; n <= n1; n++) {
            double tg = p->t0 + n / fps;
            double tval = (tg - t_prev) / (t_cur - t_prev);
            if (fabs(tval - 1.0) < 1e-6) {
                struct mp_image *out = mp_image_new_ref(cur);
                if (out) {
                    out->pts = tg;   // grid PTS (uniform output clock)
                    out->nominal_fps = p->out_fps;
                    out->pkt_duration = 1.0 / p->out_fps;
                }
                p->queue[p->queue_len++] = out;
                *grid_emitted_cur = true;
            } else if (tval > 0 && tval < 1) {
                CUdeviceptr out_buf = 0;
                if (cuMemAlloc(&out_buf, (size_t)p->eng.rgba_pitch * p->video_h)
                        != CUDA_SUCCESS) {
                    MP_ERR(f, "rife: output alloc failed\n");
                    ok = false;
                    break;
                }
                bool ok_run = scene
                    ? rife_pass_through(&p->eng, p->cuda_stream, tval > 0.5,
                                        out_buf, p->eng.rgba_pitch)
                    : rife_interpolate(&p->eng, p->cuda_stream, tval,
                                       out_buf, p->eng.rgba_pitch);
                if (!ok_run) {
                    cuMemFree(out_buf);
                    MP_ERR(f, "rife: %s failed\n", scene ? "pass-through" : "interpolate");
                    ok = false;
                    break;
                }
                struct mp_image *out = rife_make_output(f, p, out_buf,
                                                        p->video_w, p->video_h,
                                                        p->eng.rgba_pitch,
                                                        cur, tg);
                p->queue[p->queue_len++] = out;
            }
        }
    }

    // ── GPU serialization boundary ────────────────────────────────────
    // All of this pair's work is queued on our stream; sync it now. The
    // chain is host-serial, so this makes rife's work and vf_vsr's work
    // alternate strictly on the shared context's GPU (no concurrent
    // saturation). Stream-level only — a device-level sync would also wait
    // for the VO's stream-0 copies and can deadlock (Xid 109 lesson).
    // Also guarantees output buffers are complete before they're handed
    // downstream (vsr copies them; when vsr is in passthrough the VO does).
    cuStreamSynchronize(p->cuda_stream);
    if (!ok)
        return false;

    // swap staging: cur becomes prev
    CUdeviceptr tmp = p->eng.rgba_a;
    p->eng.rgba_a = p->eng.rgba_b;
    p->eng.rgba_b = tmp;

    adapt_record(p, now_ms() - t_start);
    return true;
}

// ── process ───────────────────────────────────────────────────────────

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    // 1. drain output queue: exactly one output per call
    if (p->queue_len > 0) {
        struct mp_image *out = p->queue[p->queue_pos++];
        if (p->queue_pos == p->queue_len)
            p->queue_len = p->queue_pos = 0;
        MP_DBG(f, "rife: OUT pts=%.4f src-pts=%.4f %s\n", out->pts,
               p->prev_pts,
               fabs(out->pts - p->prev_pts) < 0.001 ? "[copy]" : "[mid]");
        mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
        return;
    }

    // 2. EOF flush: emit last frame if its grid point never landed
    if (p->eof_seen) {
        if (p->have_prev && !p->cur_emitted) {
            struct mp_image *last = p->prev;
            p->prev = NULL;
            p->have_prev = false;
            p->cur_emitted = true;
            last->nominal_fps = p->out_fps;
            last->pkt_duration = 1.0 / p->out_fps;
            mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, last));
            return;
        }
        p->have_prev = false;
        mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
        return;
    }

    // 3. need input
    if (!mp_pin_out_request_data(f->ppins[0]))
        return;
    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);

    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type == MP_FRAME_EOF) {
        p->eof_seen = true;
        return;  // flush on next call (queue drains first)
    }
    if (frame.type != MP_FRAME_VIDEO) {
        MP_ERR(f, "rife: unexpected frame type %d\n", frame.type);
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    struct mp_image *cur = frame.data;
    p->frame_count++;

    // SW frames: passthrough (rife is CUDA-only; SW input = hwdec disabled)
    if (cur->imgfmt != IMGFMT_CUDA) {
        if (!p->sw_warned) {
            MP_WARN(f, "rife: software frames — passthrough\n");
            p->sw_warned = true;
        }
        p->mode = RIFE_PASSTHROUGH;
        snprintf(p->pt_reason, sizeof(p->pt_reason), "sw");
        if (p->frame_count % RIFE_STATUS_EVERY == 0)
            rife_status(f, p);
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // size change → recompute output fps / adaptive budget
    // (engine staging reconfig happens after engine ensure below)
    if (cur->w != p->in_w || cur->h != p->in_h) {
        p->in_w = cur->w;
        p->in_h = cur->h;
        if (cur->nominal_fps > 0)
            p->src_fps = cur->nominal_fps;
        if (p->opts->scale > 0)
            p->out_fps = p->opts->scale * p->src_fps;
        else if (p->opts->fps == 0)
            p->out_fps = 2 * p->src_fps;
        else
            p->out_fps = p->opts->fps;
        p->frame_budget_ms = 1000.0 / p->src_fps - RIFE_BUDGET_VSR_MS;
        if (p->frame_budget_ms < 5.0)
            p->frame_budget_ms = 5.0;
        MP_INFO(f, "rife: %dx%d src %.2f fps → out %.2f fps (%s)\n",
                cur->w, cur->h, p->src_fps, p->out_fps,
                p->opts->scale > 0 ? "benchmark" : "target");
    }

    // mode decision (re-evaluated every frame — hot updates take effect here)
    if (!decide_mode(f, p, cur)) {
        if (p->frame_count % RIFE_STATUS_EVERY == 0)
            rife_status(f, p);
        mp_pin_in_write(f->ppins[1], frame);   // passthrough: forward untouched
        return;
    }

    // engine lazy init (first active frame)
    if (!p->engine_ok) {
        if (!ensure_cuda(f, p, cur)) {
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            return;
        }
        cuCtxPushCurrent(p->cuda_ref->ctx);
        // engine shape = input size padded to 128 multiples (lite alignment,
        // mirrors build_rife_lite_engine.sh); one fixed-shape engine per size
        int ph = (cur->h + 127) / 128 * 128;
        int pw = (cur->w + 127) / 128 * 128;
        p->engine_ok = rife_init(&p->eng, p->cuda_ref->ctx, p->cuda_stream,
                                 ph, pw, f->log);
        if (p->engine_ok)
            p->engine_ok = rife_reconfig(&p->eng, cur->w, cur->h, p->cuda_stream);
        cuCtxPopCurrent(NULL);
        if (!p->engine_ok) {
            if (p->opts->scale > 0) {
                MP_ERR(f, "rife: engine init failed (benchmark)\n");
                mp_frame_unref(&frame);
                mp_filter_internal_mark_failed(f);
                return;
            }
            if (!p->degrade_warned) {
                MP_WARN(f, "rife: engine init failed — passthrough\n");
                p->degrade_warned = true;
            }
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "engine");
            if (p->frame_count % RIFE_STATUS_EVERY == 0)
                rife_status(f, p);
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }
        p->video_w = cur->w;   // size change block above skipped; stage now
        p->video_h = cur->h;
    } else if (cur->w != p->video_w || cur->h != p->video_h) {
        // size change after init: reallocate staging; reload the engine if
        // the padded shape changed (one fixed-shape engine per size)
        int ph = (cur->h + 127) / 128 * 128;
        int pw = (cur->w + 127) / 128 * 128;
        cuCtxPushCurrent(p->cuda_ref->ctx);
        bool ok = true;
        if (ph != p->eng.ph || pw != p->eng.pw) {
            rife_destroy(&p->eng);
            ok = rife_init(&p->eng, p->cuda_ref->ctx, p->cuda_stream,
                           ph, pw, f->log);
            if (!ok && p->opts->scale == 0) {
                p->mode = RIFE_PASSTHROUGH;
                snprintf(p->pt_reason, sizeof(p->pt_reason), "engine-size");
                MP_WARN(f, "rife: engine for %dx%d (pad %dx%d) not found — "
                        "passthrough\n", cur->w, cur->h, ph, pw);
            }
        }
        if (ok)
            ok = rife_reconfig(&p->eng, cur->w, cur->h, p->cuda_stream);
        cuCtxPopCurrent(NULL);
        if (!ok) {
            if (p->opts->scale > 0 || p->mode != RIFE_PASSTHROUGH) {
                mp_frame_unref(&frame);
                mp_filter_internal_mark_failed(f);
                return;
            }
            p->engine_ok = false;   // stay in passthrough (engine missing)
            p->video_w = cur->w;
            p->video_h = cur->h;
            if (p->frame_count % RIFE_STATUS_EVERY == 0)
                rife_status(f, p);
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }
        p->video_w = cur->w;
        p->video_h = cur->h;
        MP_INFO(f, "rife: reconfigure %dx%d\n", cur->w, cur->h);
    }

    // 4. first frame / pair
    if (!p->have_prev) {
        if (cur->pts == MP_NOPTS_VALUE) {
            p->mode = RIFE_PASSTHROUGH;
            snprintf(p->pt_reason, sizeof(p->pt_reason), "no-pts");
            if (!p->degrade_warned) {
                MP_WARN(f, "rife: no PTS — passthrough\n");
                p->degrade_warned = true;
            }
            mp_pin_in_write(f->ppins[1], frame);
            return;
        }
        p->t0 = cur->pts;
        p->have_prev = true;
        // prev gets its OWN ref — the frame itself is handed to the out pin
        // (ownership moves there). Sharing the pin's ref would leave two
        // owners of one ref: the next pair's mp_image_unrefp(&p->prev)
        // would free the frame while the pin/VO still hold it — freed hwdec
        // surface recycled under the VO's retained frame → GPU hangs and
        // use-after-free crashes (observed SIGSEGV in av_buffer_ref).
        p->prev = mp_image_new_ref(cur);
        p->prev_pts = cur->pts;
        p->cur_emitted = true;
        cur->nominal_fps = p->out_fps;
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    double t_prev = p->prev_pts;
    double t_cur = cur->pts;
    if (t_cur == MP_NOPTS_VALUE || t_cur <= t_prev) {
        // duplicate / reorder / missing pts: drop cur, keep prev
        MP_DBG(f, "rife: drop frame pts=%.3f (prev=%.3f)\n", t_cur, t_prev);
        mp_frame_unref(&frame);
        p->cur_emitted = true;
        return;
    }

    // Release the old prev's hwdec frame now — its RGBA already lives in
    // staging; only its pts (prev_pts) is needed for the next pair. Keeping
    // the buffer would press the nvdec surface pool (decoder stalls once
    // every surface is held). The EOF flush's frame is the LAST cur, which
    // stays referenced (p->prev = cur below) until the next pair arrives.
    mp_image_unrefp(&p->prev);

    cuCtxPushCurrent(p->cuda_ref->ctx);
    bool grid_emitted_cur = false;
    bool ok = process_pair(f, p, t_prev, t_cur, cur, &grid_emitted_cur);
    cuCtxPopCurrent(NULL);

    // cur becomes prev for the next pair — ownership transfers from the pin
    // frame to p->prev (no mp_frame_unref: that would free cur first, and
    // p->prev = cur would dangle — reading freed memory at the next pair).
    p->prev = cur;
    p->have_prev = true;
    p->prev_pts = t_cur;
    p->cur_emitted = grid_emitted_cur;
    if (!ok) {
        p->prev = NULL;
        p->have_prev = false;
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    // emit at most one queued output this call
    if (p->queue_len > 0) {
        struct mp_image *out = p->queue[p->queue_pos++];
        if (p->queue_pos == p->queue_len)
            p->queue_len = p->queue_pos = 0;
        MP_DBG(f, "rife: OUT pts=%.4f src-pts=%.4f %s\n", out->pts,
               p->prev_pts,
               fabs(out->pts - p->prev_pts) < 0.001 ? "[copy]" : "[mid]");
        // TEST: 帧内容指纹——mid 帧 vs-prev/vs-cur 应 ≈ [0.5d, 0.5d]；
        // [d, 0] = mid 复制了 cur、[0, d] = mid 复制了 prev——帧序错位
        if (p->engine_ok) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            float mp = 0, mc = 0, dsrc = 0;
            if (rife_mae_of(&p->eng, p->cuda_stream,
                            (CUdeviceptr)out->planes[0], p->eng.rgba_b, &mp) &&
                rife_mae_of(&p->eng, p->cuda_stream,
                            (CUdeviceptr)out->planes[0], p->eng.rgba_a, &mc) &&
                rife_mae_of(&p->eng, p->cuda_stream,
                            p->eng.rgba_a, p->eng.rgba_b, &dsrc))
                MP_DBG(f, "rife: MAE pts=%.4f vs-prev=%.2f vs-cur=%.2f d_src=%.2f\n",
                        out->pts, mp, mc, dsrc);
            cuCtxPopCurrent(NULL);
        }
        mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
    }
    if (p->frame_count % RIFE_STATUS_EVERY == 0)
        rife_status(f, p);
}

// ── reset / destroy / command ─────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    // Free only frames not yet handed to the out pin: queue[0..queue_pos)
    // were emitted (ownership moved to the pin — freeing them here would
    // double-free once the downstream releases them; observed canary
    // assertion on fps hot-switch mid-drain). Queue slots before queue_pos
    // are stale by design.
    for (int i = p->queue_pos; i < p->queue_len; i++)
        talloc_free(p->queue[i]);
    p->queue_len = p->queue_pos = 0;
    mp_image_unrefp(&p->prev);
    p->have_prev = false;
    p->prev_pts = 0;
    p->eof_seen = false;
    p->cur_emitted = false;
    p->t0 = 0;
    p->frame_count = 0;
    p->adapt_warmup_done = false;
    p->pair_count = 0;
    p->adapt_idx = 0;
    p->mode = RIFE_ACTIVE;   // clear one-way adaptive degradation
    p->degrade_warned = false;
    MP_INFO(f, "rife: reset\n");
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    // same ownership rule as f_reset: queue[0..queue_pos) belong to the pin
    for (int i = p->queue_pos; i < p->queue_len; i++)
        talloc_free(p->queue[i]);
    mp_image_unrefp(&p->prev);
    bool ctx_pushed = false;
    if (p->cuda_ref && p->cuda_ref->ctx)
        ctx_pushed = cuCtxPushCurrent(p->cuda_ref->ctx) == CUDA_SUCCESS;
    if (p->engine_ok)
        rife_destroy(&p->eng);
    if (p->yuv_conv_ready)
        yuv_to_rgba_destroy(&p->yuv_conv);
    if (ctx_pushed && p->cuda_init_done)
        cuStreamDestroy(p->cuda_stream);
    if (ctx_pushed)
        cuCtxPopCurrent(NULL);
    if (p->device_ref) {
        av_buffer_unref(&p->device_ref);
        p->device_ref = NULL;
    }
    if (p->cuda_ref) {
        rife_cuda_ref_unref(p->cuda_ref);
        p->cuda_ref = NULL;
    }
}

static bool parse_fps_arg(const char *arg, int *out)
{
    if (!strcmp(arg, "off"))  { *out = -1; return true; }
    if (!strcmp(arg, "auto")) { *out = 0;  return true; }
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (!end || *end || v < 1 || v > 120)
        return false;
    *out = (int)v;
    return true;
}

static bool rife_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || !cmd->arg)
        return false;
    if (strcmp(cmd->cmd, "fps") == 0) {
        int v;
        if (!parse_fps_arg(cmd->arg, &v))
            return false;
        if (p->opts->fps != v) {
            MP_INFO(f, "rife: fps %d -> %d\n", p->opts->fps, v);
            p->opts->fps = v;
            p->degrade_warned = false;
            // out_fps 只在尺寸变化时重算——fps 热切换后必须同步重算，
            // 否则沿用旧值（首帧 off → out_fps=-1 → 切 active 后 grid
            // 计算产生 0 输出 → 画面卡住）。
            if (p->src_fps > 0) {
                if (p->opts->scale > 0)
                    p->out_fps = p->opts->scale * p->src_fps;
                else if (p->opts->fps == 0)
                    p->out_fps = 2 * p->src_fps;
                else
                    p->out_fps = p->opts->fps;
            }
            // 连续性破坏：重置调度状态（prev/t0/queue，f_reset 不销毁
            // 引擎——跨 seek/切换保留），下一帧按新网格重新起算。
            f_reset(f);
        }
        return true;
    }
    if (strcmp(cmd->cmd, "scale") == 0) {
        int v = !strcmp(cmd->arg, "off") ? 0 : atoi(cmd->arg);
        if (v != 0 && v != 2 && v != 3 && v != 4)
            return false;
        if (p->opts->scale != v) {
            MP_INFO(f, "rife: scale %d -> %d\n", p->opts->scale, v);
            p->opts->scale = v;
        }
        return true;
    }
    if (strcmp(cmd->cmd, "adaptive") == 0) {
        bool v;
        if (strcmp(cmd->arg, "yes") == 0)      v = true;
        else if (strcmp(cmd->arg, "no") == 0)  v = false;
        else if (strcmp(cmd->arg, "1") == 0)   v = true;
        else if (strcmp(cmd->arg, "0") == 0)   v = false;
        else return false;
        p->opts->adaptive = v;
        return true;
    }
    return false;
}

// ── registration ──────────────────────────────────────────────────────

static const struct mp_filter_info vf_rife_filter = {
    .name      = "rife",
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = rife_command,
};

static struct mp_filter *vf_rife_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_rife_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    p->mode = RIFE_PASSTHROUGH;
    p->out_fps = 0;
    return f;
}

const struct mp_user_filter_entry vf_rife = {
    .desc = {
        .name        = "rife",
        .description = "AI frame interpolation (RIFE, TensorRT)",
        .priv_size   = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_rife_opts_def,
        .options     = vf_opts_fields,
    },
    .create = vf_rife_create,
};
