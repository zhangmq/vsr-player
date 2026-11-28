// src/mpv/video/filter/vfx_fatal.h
// Structured failure reporting for the VFX nodes (VSR / VFG / TrueHDR).
//
// Why: an engine that cannot start must not silently degrade into passthrough
// when the real cause is an exhausted GPU (the user asked for "stop playback
// and say why"), while a missing SDK or an unsupported configuration must keep
// playing at 1:1. The filters cannot decide that alone (it depends on the
// host's policy and on the free memory at that instant), so they emit one
// machine-readable line and the Qt client decides what to do with it.
//
// Format (stable, parsed by key):
//   vfx-error: node=<vsr|vfg|truehdr> reason=<class> err=<code> dims=WxH
//              free=<bytes> total=<bytes>
//
// reason classes:
//   sdk      - the VFX libraries could not be loaded (SDK missing/incomplete)
//   engine   - CreateEffect / Load / Run refused the configuration (the host
//              checks free memory to tell "GPU exhausted" from "unsupported")
//   resource - a GPU allocation failed, or the engine failed with the SDK's
//              CUDA-failure code range (-1000 and below)
//   driver   - the CUDA context could not be acquired / is unusable
//
// Host memory is deliberately not a class: the system kills a process that
// runs out of RAM, so there is no playback to stop (user, 2026-11-28).
//
// free/total are the device numbers *at the moment of failure* (bytes), read
// with the caller's CUDA context pushed. A caller without a current context
// (e.g. the context acquisition itself failed) reports 0/0 and the host falls
// back to its last known numbers.
#pragma once

#include <cuda.h>

#include "common/msg.h"
#include "filters/filter.h"

// -1000 and below is the SDK's "CUDA failure" range (NvVFX_ERR_CUDA_FAILURE and
// friends); the exact enum values are not exposed in the C headers we use, and
// the only codes we observed in practice are -1002 (Load under memory pressure)
// and -7 (parameter rejected). Treat that range as a resource problem.
#define VFX_ERR_IS_CUDA(code) ((code) <= -1000)

static inline void vfx_report_error(struct mp_filter *f, const char *node,
                                    const char *reason, long err,
                                    int w, int h)
{
    size_t free_b = 0, total_b = 0;
    if (cuMemGetInfo(&free_b, &total_b) != CUDA_SUCCESS) {
        free_b = 0;
        total_b = 0;
    }
    MP_ERR(f, "vfx-error: node=%s reason=%s err=%ld dims=%dx%d free=%zu total=%zu\n",
           node, reason, err, w, h, free_b, total_b);
}
