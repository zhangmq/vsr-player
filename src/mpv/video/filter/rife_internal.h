// rife_internal.h — RIFE inference engine plumbing (TRT engine + full-frame
// kernels). Implemented in rife_proc.c. No mp_filter concerns; owned by vf_rife.
#ifndef RIFE_INTERNAL_H
#define RIFE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <cuda.h>

#include "rife_trt.h"

struct mp_log;

struct rife_context {
    // TensorRT engine (NULL when TRT unavailable or load failed)
    struct rife_engine *engine;

    // full-frame kernels (nvrtc)
    CUmodule   module;
    CUfunction assemble_fn;    // rgba staging pair → 11ch FP16 engine input
                               //   (reflect pad + grid channels + normalize)
    CUfunction convert_fn;     // 3ch FP16 engine output → RGBA u8
    CUfunction copy_fn;        // rgba_a → out (scene-change pass-through)
    CUfunction mae_fn;         // |rgba_a - rgba_b| mean → host scalar
    bool       kernels_ready;

    // full-frame RGBA u8 staging (prev / cur), owned here. Cur is converted
    // directly from the hwdec frame (shared CUDA context — no NV12 staging
    // copy); prev was converted by the previous pair.
    CUdeviceptr rgba_a;
    CUdeviceptr rgba_b;
    int         rgba_pitch;
    int         w, h;          // current frame size

    CUdeviceptr mae_partial;   // 256 floats, scene-change block sums
    bool        mae_partial_alloc;

    // padded engine dims (PH/PW, multiples of 128 — lite alignment)
    int ph, pw;

    bool configured;

    struct mp_log *log;        // mpv log for engine/kernel diagnostics
};

// ctx must be current; loads engine (searches RIFE_LIBDIR etc), compiles kernels.
// ph/pw = padded engine dims for this video size (128 multiples).
bool rife_init(struct rife_context *c, CUcontext ctx, CUstream stream,
               int ph, int pw, struct mp_log *log);
void rife_destroy(struct rife_context *c);

// Reallocate staging buffers on frame-size change (engine dims unchanged —
// ph/pw are fixed at init by the video size).
bool rife_reconfig(struct rife_context *c, int w, int h, CUstream stream);

// Scene-change detection: mean |a-b| over the frame region (0-255 scale).
// Returns true when the mean exceeds thresh. Synchronizes the stream for the
// host read — call only once per pair before interpolation.
bool rife_scene_change(struct rife_context *c, CUstream stream, double thresh);

// Interpolate one output frame: input frames are the two RGBA staging buffers
// (rgba_a = prev, rgba_b = cur), tval in (0,1]. Output written to out_rgba
// (full-frame RGBA u8, out_pitch). ctx must be current. Enqueues all work on
// `stream` and returns once enqueued (NOT synchronized — the caller syncs
// the stream at its GPU serialization boundary).
bool rife_interpolate(struct rife_context *c, CUstream stream,
                      double tval, CUdeviceptr out_rgba, int out_pitch);

// Scene-change pass-through: copy one endpoint (from_cur ? rgba_b : rgba_a)
// to out_rgba, no inference. minterpolate semantics: duplicate the temporally
// nearer endpoint (alpha > 0.5 → next, else previous) — copying the far frame
// would show content from the wrong side of the cut.
bool rife_pass_through(struct rife_context *c, CUstream stream, bool from_cur,
                       CUdeviceptr out_rgba, int out_pitch);

// TEST: frame-content fingerprint — mean |a - b| over the frame region
// (0-255 scale), host-read (syncs the stream). Used to detect abnormal
// frames in the output sequence (flicker = content-level anomaly).
bool rife_mae_of(struct rife_context *c, CUstream stream,
                 CUdeviceptr a, CUdeviceptr b, float *out);

#endif // RIFE_INTERNAL_H
