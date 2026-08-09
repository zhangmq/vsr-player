// rife_proc.c — RIFE inference plumbing: engine file location, full-frame
// kernels (nvrtc), per-frame interpolation. Model: rife_v4.25 lite (11ch,
// vs-mlrt external-models conversion), fixed-shape FP16 TRT engine
// ([1,11,PH,PW] in / [1,3,PH,PW] out, PH/PW multiples of 128).
//
// Zero-copy path:
//   rife_assemble (nvrtc):  rgba_a/rgba_b full frame → 11ch engine input
//                           (reflect pad to PH/PW + grid channels + normalize)
//   rife_engine_run (TRT):  inference (single full-frame enqueue)
//   rife_convert   (nvrtc): engine output → full-frame RGBA u8
// All on the caller's stream; the caller synchronizes the stream at its GPU
// serialization boundary (vf_rife process_pair) — rife_interpolate is async.
//
// Scene-change pass-through: rife_scene_change computes mean |a-b| on the
// staging pair (host read, one sync per pair); on detection the caller emits
// rife_pass_through (rgba_a copy) instead of interpolating — vs-mlrt
// SceneChangeNext convention.
#include "config.h"   // HAVE_TRT

#include "rife_internal.h"

#include "common/msg.h"

#include <nvrtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Engine file search ──────────────────────────────────────────────────

// Probe each candidate dir for rife_lite_fp16_{ph}x{pw}.engine; first hit
// wins. Order: RIFE_LIBDIR → installed (~/.local/lib/vsr-player) → dev
// (third_party/rife) → build tree (build/tests/fruc).
static bool rife_locate_engine(char *path, size_t bufsz, int ph, int pw)
{
    char name[64];
    snprintf(name, sizeof(name), "rife_lite_fp16_%dx%d.engine", ph, pw);
    const char *env = getenv("RIFE_LIBDIR");
    if (env && *env) {
        snprintf(path, bufsz, "%s/%s", env, name);
        if (access(path, F_OK) == 0) return true;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(path, bufsz, "%s/.local/lib/vsr-player/%s", home, name);
        if (access(path, F_OK) == 0) return true;
    }
    const char *cwd = getenv("PWD");
    if (cwd && strstr(cwd, "vsr-player")) {
        snprintf(path, bufsz, "%s/third_party/rife/%s", cwd, name);
        if (access(path, F_OK) == 0) return true;
        snprintf(path, bufsz, "%s/build/tests/fruc/%s", cwd, name);
        if (access(path, F_OK) == 0) return true;
    }
    return false;
}

// ── Full-frame kernels (nvrtc, mirrors yuv_to_rgba.c pattern) ──────────
//
// rife_assemble: read (sx,sy) from the frame with reflect pad at the bottom/
// right edges (lite model has no internal padding — the padded band must be
// a mirror of the content, not zeros: zero-filled edges measure 2.2-3.5× the
// error of the interior, vs-mlrt uses reflect). Grid channels follow
// vsmlrt.get_rife_input exactly (bit-exact verified against the v2 model):
//   GH = 2x/(PW-1)-1, GV = 2y/(PH-1)-1, MH = 2/(PW-1), MW = 2/(PH-1)
static const char *kKernelSrc =
"// FP16 conversion without <cuda_fp16.h> (NVRTC has no default include path)\n"
"__device__ unsigned short rife_f2h(float f) {\n"
"    unsigned short h; asm(\"cvt.rn.f16.f32 %0, %1;\" : \"=h\"(h) : \"f\"(f));\n"
"    return h;\n"
"}\n"
"__device__ float rife_h2f(unsigned short h) {\n"
"    float f; asm(\"cvt.f32.f16 %0, %1;\" : \"=f\"(f) : \"h\"(h));\n"
"    return f;\n"
"}\n"
"extern \"C\" __global__ void rife_assemble(\n"
"    const unsigned char* __restrict__ rgba_a, int a_pitch,\n"
"    const unsigned char* __restrict__ rgba_b, int b_pitch,\n"
"    unsigned short* __restrict__ in11,\n"
"    int frame_w, int frame_h, int PW, int PH, float tval)\n"
"{\n"
"    int idx = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int total = PW * PH;\n"
"    if (idx >= total) return;\n"
"    int x = idx % PW;\n"
"    int y = idx / PW;\n"
"    // reflect at the padded edges (pad <= frame-1 by construction)\n"
"    int sx = x < frame_w ? x : 2 * (frame_w - 1) - x;\n"
"    int sy = y < frame_h ? y : 2 * (frame_h - 1) - y;\n"
"    const unsigned char* pa = rgba_a + sy * a_pitch + sx * 4;\n"
"    const unsigned char* pb = rgba_b + sy * b_pitch + sx * 4;\n"
"    size_t plane = (size_t)PW * PH;\n"
"    unsigned short* d = in11 + idx;\n"
"    float inv = 1.0f / 255.0f;\n"
"    d[0]            = rife_f2h(pa[0] * inv);\n"
"    d[plane]        = rife_f2h(pa[1] * inv);\n"
"    d[2*plane]      = rife_f2h(pa[2] * inv);\n"
"    d[3*plane]      = rife_f2h(pb[0] * inv);\n"
"    d[4*plane]      = rife_f2h(pb[1] * inv);\n"
"    d[5*plane]      = rife_f2h(pb[2] * inv);\n"
"    d[6*plane]      = rife_f2h(tval);\n"
"    d[7*plane]      = rife_f2h(2.0f * x / (PW - 1) - 1.0f);\n"
"    d[8*plane]      = rife_f2h(2.0f * y / (PH - 1) - 1.0f);\n"
"    d[9*plane]      = rife_f2h(2.0f / (PW - 1));\n"
"    d[10*plane]     = rife_f2h(2.0f / (PH - 1));\n"
"}\n"
"extern \"C\" __global__ void rife_convert(\n"
"    const unsigned short* __restrict__ out3,\n"
"    unsigned char* __restrict__ rgba, int rgba_pitch,\n"
"    int frame_w, int frame_h, int PW, int PH)\n"
"{\n"
"    int idx = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int total = frame_w * frame_h;\n"
"    if (idx >= total) return;\n"
"    int x = idx % frame_w;\n"
"    int y = idx / frame_w;\n"
"    size_t plane = (size_t)PW * PH;   // engine output plane stride\n"
"    const unsigned short* p = out3 + (size_t)y * PW + x;\n"
"    unsigned char* o = rgba + y * rgba_pitch + x * 4;\n"
"    o[0] = (unsigned char)(fminf(fmaxf(rife_h2f(p[0]), 0.0f), 1.0f) * 255.0f + 0.5f);\n"
"    o[1] = (unsigned char)(fminf(fmaxf(rife_h2f(p[plane]), 0.0f), 1.0f) * 255.0f + 0.5f);\n"
"    o[2] = (unsigned char)(fminf(fmaxf(rife_h2f(p[2*plane]), 0.0f), 1.0f) * 255.0f + 0.5f);\n"
"    o[3] = 255;\n"
"}\n"
"extern \"C\" __global__ void rife_copy(\n"
"    const unsigned char* __restrict__ rgba_a, int a_pitch,\n"
"    unsigned char* __restrict__ rgba, int rgba_pitch,\n"
"    int frame_w, int frame_h)\n"
"{\n"
"    int idx = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int total = frame_w * frame_h;\n"
"    if (idx >= total) return;\n"
"    int x = idx % frame_w;\n"
"    int y = idx / frame_w;\n"
"    const unsigned char* s = rgba_a + y * a_pitch + x * 4;\n"
"    unsigned char* o = rgba + y * rgba_pitch + x * 4;\n"
"    o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;\n"
"}\n"
"extern \"C\" __global__ void rife_mae(\n"
"    const unsigned char* __restrict__ rgba_a, int a_pitch,\n"
"    const unsigned char* __restrict__ rgba_b, int b_pitch,\n"
"    float* __restrict__ partial, int frame_w, int frame_h)\n"
"{\n"
"    int total = frame_w * frame_h;\n"
"    float sum = 0.0f;\n"
"    for (int idx = blockIdx.x * blockDim.x + threadIdx.x; idx < total;\n"
"         idx += gridDim.x * blockDim.x) {\n"
"        int x = idx % frame_w;\n"
"        int y = idx / frame_w;\n"
"        const unsigned char* a = rgba_a + y * a_pitch + x * 4;\n"
"        const unsigned char* b = rgba_b + y * b_pitch + x * 4;\n"
"        sum += abs((int)a[0] - b[0]) + abs((int)a[1] - b[1])\n"
"             + abs((int)a[2] - b[2]);\n"
"    }\n"
"    partial[blockIdx.x] = sum;\n"
"}\n";

static bool rife_compile_kernels(struct rife_context *c, CUcontext ctx)
{
    if (c->kernels_ready) return true;

    nvrtcProgram prog;
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc, "rife_full", 0, NULL, NULL);
    if (res != NVRTC_SUCCESS) {
        mp_err(c->log, "rife: nvrtcCreateProgram failed: %s\n", nvrtcGetErrorString(res));
        return false;
    }

    int major = 0, minor = 0;
    CUdevice dev;
    cuCtxGetDevice(&dev);
    cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);

    char arch[32];
    snprintf(arch, sizeof(arch), "--gpu-architecture=compute_%d%d", major, minor);
    const char *opts[] = {arch, "--use_fast_math"};
    res = nvrtcCompileProgram(prog, 2, opts);
    if (res != NVRTC_SUCCESS) {
        size_t log_size;
        nvrtcGetProgramLogSize(prog, &log_size);
        char *log = (char *)malloc(log_size);
        nvrtcGetProgramLog(prog, log);
        mp_err(c->log, "rife: NVRTC compile failed:\n%s\n", log);
        free(log);
        nvrtcDestroyProgram(&prog);
        return false;
    }

    size_t ptx_size;
    nvrtcGetPTXSize(prog, &ptx_size);
    char *ptx = (char *)malloc(ptx_size);
    nvrtcGetPTX(prog, ptx);
    nvrtcDestroyProgram(&prog);

    CUresult cu_res = cuModuleLoadData(&c->module, ptx);
    free(ptx);
    if (cu_res != CUDA_SUCCESS) {
        mp_err(c->log, "rife: cuModuleLoadData failed (%d)\n", cu_res);
        return false;
    }
    if (cuModuleGetFunction(&c->assemble_fn, c->module, "rife_assemble") != CUDA_SUCCESS ||
        cuModuleGetFunction(&c->convert_fn, c->module, "rife_convert") != CUDA_SUCCESS ||
        cuModuleGetFunction(&c->copy_fn, c->module, "rife_copy") != CUDA_SUCCESS ||
        cuModuleGetFunction(&c->mae_fn, c->module, "rife_mae") != CUDA_SUCCESS) {
        mp_err(c->log, "rife: cuModuleGetFunction failed\n");
        return false;
    }
    c->kernels_ready = true;
    return true;
}

// ── Lifecycle ──────────────────────────────────────────────────────────

bool rife_init(struct rife_context *c, CUcontext ctx, CUstream stream,
               int ph, int pw, struct mp_log *log)
{
    memset(c, 0, sizeof(*c));
    c->log = log;
    c->ph = ph;
    c->pw = pw;

    // engine file (data artifact, shipped alongside the app)
    char path[1024];
    if (!rife_locate_engine(path, sizeof(path), ph, pw)) {
        mp_warn(c->log, "rife: engine rife_lite_fp16_%dx%d.engine not found "
                "(RIFE_LIBDIR / ~/.local/lib/vsr-player / third_party/rife / "
                "build/tests/fruc) — passthrough\n", ph, pw);
        return false;
    }
    (void)stream;

#ifdef HAVE_TRT
    c->engine = rife_engine_load(path, NULL);
    if (!c->engine)
        mp_err(c->log, "rife: engine load failed (%s) — passthrough\n", path);
#else
    mp_err(c->log, "rife: built without TensorRT — passthrough\n");
#endif
    if (!c->engine)
        return false;

    if (rife_engine_height(c->engine) != ph || rife_engine_width(c->engine) != pw) {
        mp_err(c->log, "rife: engine dims %dx%d != requested %dx%d — passthrough\n",
               rife_engine_height(c->engine), rife_engine_width(c->engine), ph, pw);
        rife_engine_destroy(c->engine);
        c->engine = NULL;
        return false;
    }

    if (!rife_compile_kernels(c, ctx)) {
        mp_err(c->log, "rife: kernel compile failed — passthrough\n");
        return false;
    }
    mp_info(c->log, "rife: engine ready (%s)\n", path);
    return true;
}

void rife_destroy(struct rife_context *c)
{
    if (!c) return;
    if (c->rgba_a) cuMemFree(c->rgba_a);
    if (c->rgba_b) cuMemFree(c->rgba_b);
    c->rgba_a = c->rgba_b = 0;
    if (c->mae_partial) cuMemFree(c->mae_partial);
    c->mae_partial = 0;
    c->mae_partial_alloc = false;
    if (c->module) cuModuleUnload(c->module);
    c->module = 0;
    c->kernels_ready = false;
#ifdef HAVE_TRT
    rife_engine_destroy(c->engine);
#endif
    c->engine = NULL;
    c->configured = false;
}

bool rife_reconfig(struct rife_context *c, int w, int h, CUstream stream)
{
    if (c->configured && c->w == w && c->h == h)
        return true;
    if (!c->engine)
        return false;

    if (c->rgba_a) cuMemFree(c->rgba_a);
    if (c->rgba_b) cuMemFree(c->rgba_b);

    int pitch = (w * 4 + 31) & ~31;  // 32B aligned, matches yuv_to_rgba output
    if (cuMemAlloc(&c->rgba_a, (size_t)pitch * h) != CUDA_SUCCESS ||
        cuMemAlloc(&c->rgba_b, (size_t)pitch * h) != CUDA_SUCCESS) {
        mp_err(c->log, "rife: staging alloc failed\n");
        c->rgba_a = c->rgba_b = 0;
        return false;
    }
    c->rgba_pitch = pitch;
    c->w = w; c->h = h;
    c->configured = true;
    (void)stream;
    return true;
}

// ── Scene change (mean |a-b|, host read) ───────────────────────────────

bool rife_scene_change(struct rife_context *c, CUstream stream, double thresh)
{
    if (!c->configured)
        return false;

    // stage-1 block sums → host read (one sync per pair — the caller decides
    // before enqueuing interpolation work, so this sync is not on the
    // interpolation path)
    const int blocks = 256, threads = 256;
    if (!c->mae_partial_alloc) {
        if (cuMemAlloc(&c->mae_partial, blocks * sizeof(float)) != CUDA_SUCCESS)
            return false;
        c->mae_partial_alloc = true;
    }
    void *args[] = {&c->rgba_a, &c->rgba_pitch, &c->rgba_b, &c->rgba_pitch,
                    &c->mae_partial, &c->w, &c->h};
    if (cuLaunchKernel(c->mae_fn, blocks, 1, 1, threads, 1, 1, 0, stream,
                       args, NULL) != CUDA_SUCCESS)
        return false;
    float sums[blocks];
    if (cuMemcpyDtoHAsync(sums, c->mae_partial, blocks * sizeof(float), stream)
            != CUDA_SUCCESS)
        return false;
    if (cuStreamSynchronize(stream) != CUDA_SUCCESS)
        return false;
    double total = 0;
    for (int i = 0; i < blocks; i++)
        total += sums[i];
    return total / ((double)c->w * c->h * 3) > thresh;
}

// ── Interpolation ──────────────────────────────────────────────────────

bool rife_interpolate(struct rife_context *c, CUstream stream,
                      double tval, CUdeviceptr out_rgba, int out_pitch)
{
    if (!c->engine || !c->configured)
        return false;

    void *d_in = rife_engine_input(c->engine);
    void *d_out = rife_engine_output(c->engine);
    if (!d_in || !d_out)
        return false;

    int total = c->pw * c->ph;
    float tv = (float)tval;
    void *args_assemble[] = {
        &c->rgba_a, &c->rgba_pitch,
        &c->rgba_b, &c->rgba_pitch,
        &d_in,
        &c->w, &c->h, &c->pw, &c->ph, &tv,
    };
    int blocks = (total + 255) / 256;
    if (cuLaunchKernel(c->assemble_fn, blocks, 1, 1, 256, 1, 1, 0, stream,
                       args_assemble, NULL) != CUDA_SUCCESS) {
        mp_err(c->log, "rife: assemble failed\n");
        return false;
    }

    if (!rife_engine_run(c->engine, stream)) {
        mp_err(c->log, "rife: TRT enqueue failed\n");
        return false;
    }

    int ftotal = c->w * c->h;
    void *args_convert[] = {
        &d_out,
        &out_rgba, &out_pitch,
        &c->w, &c->h, &c->pw, &c->ph,
    };
    if (cuLaunchKernel(c->convert_fn, (ftotal + 255) / 256, 1, 1,
                       256, 1, 1, 0, stream, args_convert, NULL) != CUDA_SUCCESS) {
        mp_err(c->log, "rife: convert failed\n");
        return false;
    }
    // Async: enqueue only. The caller synchronizes the stream at its GPU
    // serialization boundary (vf_rife process_pair) — stream-level, never
    // device-level (a device sync would also wait for the VO's stream-0
    // copies and can deadlock — Xid 109 lesson).
    return true;
}

// ── Scene-change pass-through (prev frame copy) ────────────────────────

bool rife_pass_through(struct rife_context *c, CUstream stream,
                       CUdeviceptr out_rgba, int out_pitch)
{
    if (!c->configured)
        return false;
    int ftotal = c->w * c->h;
    void *args[] = {&c->rgba_a, &c->rgba_pitch, &out_rgba, &out_pitch,
                    &c->w, &c->h};
    return cuLaunchKernel(c->copy_fn, (ftotal + 255) / 256, 1, 1,
                          256, 1, 1, 0, stream, args, NULL) == CUDA_SUCCESS;
}
