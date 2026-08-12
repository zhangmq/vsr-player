// cuda_shared.h — CUDA helpers shared by the filter chain (vf_vsr, vf_rife).
#pragma once

#include <cuda.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/dict.h>

#include "common/msg.h"
#include "video/mp_image.h"

// Borrow the CUDA context backing a hardware frame: mpv's AVHWFramesContext
// → device → AVCUDADeviceContext.cuda_ctx. NULL for software frames.
//
// Single-context design: the filters run on the hwdec's context instead of
// creating their own (CUDA best practice — multiple contexts time-slice and
// caused GPU hangs alongside vf_rife's TensorRT context). The hwdec context
// is alive as long as a frame (or the filter's frames-ctx ref) holds it.
static inline CUcontext mp_cuda_ctx_from_mpi(const struct mp_image *mpi)
{
    if (!mpi || !mpi->hwctx)
        return NULL;
    AVHWFramesContext *fctx = (AVHWFramesContext *)mpi->hwctx->data;
    if (!fctx || !fctx->device_ctx)
        return NULL;
    // FFmpeg ≥7: device_ctx is a direct pointer (the ref is internal)
    AVHWDeviceContext *dctx = fctx->device_ctx;
    if (dctx->type != AV_HWDEVICE_TYPE_CUDA)
        return NULL;
    AVCUDADeviceContext *cd = (AVCUDADeviceContext *)dctx->hwctx;
    return cd ? cd->cuda_ctx : NULL;
}

// The stream the hwdec's decoder writes frames on (FFmpeg's CUDA device
// context stream — nvdec's pool copies). NULL if unavailable. A filter that
// reads hwdec frame memory with its own kernels must order its reads after
// the decode work on this stream (record on this stream, wait on its own),
// otherwise the GPU can hang on the read/write race (observed: first pair
// nondeterministically hangs at the stream sync, GPU idle).
static inline CUstream mp_cuda_stream_from_mpi(const struct mp_image *mpi)
{
    if (!mpi || !mpi->hwctx)
        return NULL;
    AVHWFramesContext *fctx = (AVHWFramesContext *)mpi->hwctx->data;
    if (!fctx || !fctx->device_ctx)
        return NULL;
    // FFmpeg ≥7: device_ctx is a direct pointer (the ref is internal)
    AVHWDeviceContext *dctx = fctx->device_ctx;
    if (dctx->type != AV_HWDEVICE_TYPE_CUDA)
        return NULL;
    AVCUDADeviceContext *cd = (AVCUDADeviceContext *)dctx->hwctx;
    return cd ? cd->stream : NULL;
}

// ── Shared CUDA context ownership (single-context design) ─────────────────
// The CUDA context lives on a reference-counted struct carried as the opaque
// of each filter output frame's bufs[0] (alongside the CUDA memory itself).
// The creator (vf_hwup's self-created ctx, or a filter's SW fallback) sets
// owned=true and destroys the context when refs hit zero. Borrowers (vf_rife,
// vf_vsr reading upstream frames) acquire the SAME struct — the context stays
// alive as long as ANY frame anywhere in the chain references it. This fixes
// the life-cycle overlap where the ctx carrier was only held by the filter
// (f_destroy → carrier gone → late frame release pushed a destroyed ctx).
//
// Borrowed hwdec contexts (frames whose bufs[0] opaque is NOT one of ours —
// mpv's decoder frames) fall back to a per-borrower struct holding
// ctx_holder = av_buffer_ref(frame hwctx), released only when refs hit zero.
// ffmpeg n9.0 adopted-current contexts are NOT destroyed on device uninit, so
// the holder keeps the driver context valid for the borrower's whole use.
#define MP_CUDA_CTX_REF_MAGIC 0x43555841  // "CUXA"

struct mp_cuda_ctx_ref {
    uint32_t    magic;
    CUcontext   ctx;
    bool        owned;        // creator destroys on refs==0; borrowers never
    atomic_int  refs;         // 1 = holder (filter) + 1 per referencing frame
    AVBufferRef *ctx_holder;  // borrower of a non-shared (hwdec) ctx: keeps
                              // the source frames ctx alive; NULL for shared
                              // structs and owned (self-created) contexts
};

static inline struct mp_cuda_ctx_ref *mp_cuda_ctx_ref_from_mpi(
    const struct mp_image *mpi)
{
    if (!mpi || !mpi->bufs[0])
        return NULL;
    // AVBuffer's opaque is only reachable via the API (the struct itself is
    // opaque). Decoder buffers are av_buffer_alloc'd → opaque NULL → the
    // magic check keeps foreign buffers out.
    struct mp_cuda_ctx_ref *r =
        (struct mp_cuda_ctx_ref *)av_buffer_get_opaque(mpi->bufs[0]);
    return r && r->magic == MP_CUDA_CTX_REF_MAGIC ? r : NULL;
}

static inline void mp_cuda_ctx_ref_acquire(struct mp_cuda_ctx_ref *r)
{
    atomic_fetch_add(&r->refs, 1);
}

// Last release: no CUDA driver call must be in flight on this context
// (free_cuda_buf has already popped; f_destroy pops before releasing).
// Borrowed contexts are never destroyed — the driver context outlives the
// struct via its true owner (upstream shared struct / hwdec frames held by
// ctx_holder). owned contexts are destroyed here.
static inline void mp_cuda_ctx_ref_release(struct mp_cuda_ctx_ref *r)
{
    if (!r)
        return;
    if (atomic_fetch_sub(&r->refs, 1) != 1)
        return;
    if (r->ctx_holder) {
        av_buffer_unref(&r->ctx_holder);
        r->ctx_holder = NULL;
    }
    if (r->owned)
        cuCtxDestroy(r->ctx);
    free(r);
}

// Ensure a CUDA frames ctx (format=CUDA, given sw_format) of size w×h on the
// current CUDA context, recreated on size/format/context change. Shared by
// vf_hwup (NV12/P010 upload frames) and vf_vsr / vf_rife (RGBA output
// frames). A frame's hwctx sw_format MUST match the frame's subfmt: hwdownload
// parses frame data per hwctx sw_format, and borrowing an NV12 hwctx for RGBA
// frames crashes (mp_image_hw_download assert, observed with SW-input
// hwdownload — no hwdec → VO lacks CUDA interop → chain-tail hwdownload).
// Needs log for error reporting. Returns false on alloc failure (state
// unchanged or partially freed — caller treats as fatal).
static inline bool mp_cuda_ensure_frames(struct mp_log *log, CUcontext ctx,
                                         enum AVPixelFormat swfmt,
                                         int w, int h,
                                         AVBufferRef **dev_ref_out,
                                         AVBufferRef **frames_ref_out,
                                         CUcontext *frames_ctx_out)
{
    if (*frames_ref_out) {
        AVHWFramesContext *fctx = (AVHWFramesContext *)(*frames_ref_out)->data;
        if (fctx->width == w && fctx->height == h &&
            fctx->sw_format == swfmt && *frames_ctx_out == ctx)
            return true;
        av_buffer_unref(frames_ref_out);
        *frames_ref_out = NULL;
    }

    if (*dev_ref_out) {
        av_buffer_unref(dev_ref_out);
        *dev_ref_out = NULL;
    }

    // ffmpeg ≥7's cuda_device_create IGNORES a pre-set AVCUDADeviceContext
    // .cuda_ctx (cuda_context_init overwrites it: default branch creates a
    // fresh context and uninit destroys it) — silently breaking the
    // single-context design (one extra context per self-made device). Pass
    // current_ctx=1 with `ctx` pushed so the device ADOPTS our context, and
    // uninit leaves it alone (borrowed context, no cuCtxDestroy).
    AVBufferRef *dev = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "current_ctx", "1", 0);
    cuCtxPushCurrent(ctx);
    int rc = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_CUDA, NULL,
                                    opts, 0);
    cuCtxPopCurrent(NULL);
    av_dict_free(&opts);
    if (rc < 0 || !dev) {
        mp_err(log, "cuda_shared: av_hwdevice_ctx_create failed (%d)\n", rc);
        return false;
    }
    *dev_ref_out = dev;

    AVBufferRef *frames = av_hwframe_ctx_alloc(dev);
    if (!frames) {
        mp_err(log, "cuda_shared: av_hwframe_ctx_alloc failed\n");
        return false;
    }
    AVHWFramesContext *fctx = (AVHWFramesContext *)frames->data;
    fctx->format    = AV_PIX_FMT_CUDA;
    fctx->sw_format = swfmt;
    fctx->width     = w;
    fctx->height    = h;

    if (av_hwframe_ctx_init(frames) < 0) {
        mp_err(log, "cuda_shared: av_hwframe_ctx_init failed\n");
        av_buffer_unref(&frames);
        return false;
    }
    *frames_ref_out = frames;
    *frames_ctx_out = ctx;
    return true;
}
