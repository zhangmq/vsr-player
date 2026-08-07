// cuda_shared.h — CUDA helpers shared by the filter chain (vf_vsr, vf_rife).
#pragma once

#include <cuda.h>
#include <stdbool.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

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

// Hold an AVBufferRef on the frames context so the device's CUDA context
// stays alive for the filter's own lifetime (between frames, f_reset,
// f_destroy). No-op for software frames. Returns false on alloc failure.
static inline bool mp_cuda_ref_device(struct mp_image *mpi,
                                      AVBufferRef **frames_ref_out)
{
    if (!mpi->hwctx)
        return true;   // SW: nothing to hold
    AVBufferRef *ref = av_buffer_ref(mpi->hwctx);
    if (!ref)
        return false;
    av_buffer_unref(frames_ref_out);
    *frames_ref_out = ref;
    return true;
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
