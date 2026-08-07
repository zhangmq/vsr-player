// rife_trt.h — TensorRT engine wrapper, pure C interface.
// Implemented in rife_trt.cpp (links libnvinfer at build time, gated by HAVE_TRT).
// vf_rife calls these functions directly (static link into libmpv); when TRT
// headers/libs are absent at build time, rife_engine_load returns NULL and the
// filter degrades to passthrough.
//
// Zero-copy design: the engine owns device buffers (d_in/d_out); tile kernels
// in rife_proc.c write the 7-channel tile directly into rife_engine_input()
// and read the output from rife_engine_output() — no H2D/D2H per tile.
#ifndef RIFE_TRT_H
#define RIFE_TRT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rife_engine;

// log callback: severity 0=INFO 1=WARN 2=ERROR (TRT Severity mapping)
typedef void (*rife_log_fn)(int severity, const char *msg);

// Load engine from `path` (serialized plan file). Returns NULL on failure.
// Fixed shape (1,7,512,512) fp32 in / (1,3,512,512) fp32 out.
// Device buffers are allocated on the CUDA context current at load time —
// the caller must push its context before calling this.
struct rife_engine *rife_engine_load(const char *path, rife_log_fn log);

// Run inference on the current input buffer contents. `stream` must belong
// to the context that is current at call time (same one that allocated
// the buffers). Returns false on failure.
bool rife_engine_run(struct rife_engine *e, void *stream);

// Device buffer access (for tile kernels). Ownership stays with the engine.
void *rife_engine_input(struct rife_engine *e);
void *rife_engine_output(struct rife_engine *e);

// Fixed engine dims (512) — the tile size.
int rife_engine_size(struct rife_engine *e);

void rife_engine_destroy(struct rife_engine *e);

#ifdef __cplusplus
}
#endif

#endif // RIFE_TRT_H
