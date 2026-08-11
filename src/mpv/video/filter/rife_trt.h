// rife_trt.h — TensorRT engine wrapper, pure C interface.
// Implemented in rife_trt.cpp (links libnvinfer at build time, gated by HAVE_TRT).
// vf_rife calls these functions directly (static link into libmpv); when TRT
// headers/libs are absent at build time, rife_engine_load returns NULL and the
// filter degrades to passthrough.
//
// Zero-copy design: the engine owns device buffers (d_in/d_out); the
// assemble kernel in rife_proc.c writes the 11-channel full-frame input
// directly into rife_engine_input() and the convert kernel reads the output
// from rife_engine_output() — no H2D/D2H per frame.
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
// Input/output shapes and dtypes are read from the engine itself (fixed
// shape engines built by build_rife_lite_engine.sh: [1,11,PH,PW] in /
// [1,3,PH,PW] out, FP16). Device buffers are allocated on the CUDA context
// current at load time — the caller must push its context before calling.
struct rife_engine *rife_engine_load(const char *path, rife_log_fn log);

// Run inference on the current input buffer contents. `stream` must belong
// to the context that is current at call time (same one that allocated
// the buffers). Returns false on failure.
bool rife_engine_run(struct rife_engine *e, void *stream);

// Device buffer access (for assemble/convert kernels). Ownership stays with
// the engine. Input is 11×PH×PW, output 3×PH×PW (FP16 elements).
void *rife_engine_input(struct rife_engine *e);
void *rife_engine_output(struct rife_engine *e);

// Engine spatial dims. Fixed-shape engine: the built padded size (video W/H
// rounded up to 128). Dynamic-shape engine (full variant): the current input
// shape (set via rife_engine_set_shape, initial = profile opt).
int rife_engine_height(struct rife_engine *e);   // PH
int rife_engine_width(struct rife_engine *e);    // PW

// Profile maximum dims (dynamic engine: max profile; fixed: built dims).
// Videos larger than this must be rejected (passthrough).
int rife_engine_max_height(struct rife_engine *e);
int rife_engine_max_width(struct rife_engine *e);

// Set the input shape for dynamic-shape engines (full variant). Buffers are
// allocated at profile max at load time, so no reallocation happens here.
// Returns false when h/w exceed the profile max (caller degrades).
bool rife_engine_set_shape(struct rife_engine *e, int h, int w);

// FP16 engine? (all rife engines are; used to pick the input element size)
bool rife_engine_half(struct rife_engine *e);

void rife_engine_destroy(struct rife_engine *e);

#ifdef __cplusplus
}
#endif

#endif // RIFE_TRT_H
