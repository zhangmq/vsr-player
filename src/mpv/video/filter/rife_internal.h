// rife_internal.h — RIFE inference engine plumbing (TRT engine + tile kernels).
// Implemented in rife_proc.c. No mp_filter concerns; owned by vf_rife.
#ifndef RIFE_INTERNAL_H
#define RIFE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <cuda.h>

#include "rife_trt.h"

struct mp_log;

#define RIFE_TILE 512  // engine fixed square input size

struct rife_context {
    // TensorRT engine (NULL when TRT unavailable or load failed)
    struct rife_engine *engine;

    // tile kernels (nvrtc)
    CUmodule   module;
    CUfunction assemble_fn;
    CUfunction convert_fn;
    bool       kernels_ready;

    // full-frame RGBA u8 staging (prev / cur), owned here. Cur is converted
    // directly from the hwdec frame (shared CUDA context — no NV12 staging
    // copy); prev was converted by the previous pair.
    CUdeviceptr rgba_a;
    CUdeviceptr rgba_b;
    int         rgba_pitch;
    int         w, h;          // current frame size

    int tiles_x, tiles_y;

    bool configured;

    struct mp_log *log;        // mpv log for engine/kernel diagnostics
};

// ctx must be current; loads engine (searches RIFE_LIBDIR etc), compiles kernels.
bool rife_init(struct rife_context *c, CUcontext ctx, CUstream stream,
               struct mp_log *log);
void rife_destroy(struct rife_context *c);

// Reallocate staging buffers on frame-size change; recompute tile grid.
bool rife_reconfig(struct rife_context *c, int w, int h, CUstream stream);

// Interpolate one output frame: input frames are the two RGBA staging buffers
// (rgba_a = prev, rgba_b = cur), tval in (0,1]. Output written to out_rgba
// (full-frame RGBA u8, out_pitch). ctx must be current. Enqueues all work on
// `stream` and returns once enqueued (NOT synchronized — the caller syncs
// the stream at its GPU serialization boundary).
bool rife_interpolate(struct rife_context *c, CUstream stream,
                      double tval, CUdeviceptr out_rgba, int out_pitch);

#endif // RIFE_INTERNAL_H
