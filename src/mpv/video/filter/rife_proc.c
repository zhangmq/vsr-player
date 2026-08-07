// rife_proc.c — RIFE inference plumbing: engine file location, tile kernels
// (nvrtc), per-frame interpolation. Model: rife_v4.25 (vs-mlrt v2 conversion),
// fixed 512×512 TRT engine, 7ch input [A RGB(3), B RGB(3), t(1)].
//
// Zero-copy path per tile:
//   rife_assemble (nvrtc):  rgba_a/rgba_b tile region → engine input buffer
//   rife_engine_run (TRT):  inference
//   rife_convert   (nvrtc): engine output → full-frame RGBA u8
// All on the caller's stream; the caller synchronizes the stream at its GPU
// serialization boundary (vf_rife process_pair) — rife_interpolate is async.
#include "config.h"   // HAVE_TRT

#include "rife_internal.h"

#include "common/msg.h"

#include <nvrtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Engine file search ──────────────────────────────────────────────────

// Probe each candidate dir for the engine file; first hit wins.
// Order: RIFE_LIBDIR → installed (~/.local/lib/vsr-player) → dev (third_party/rife).
static bool rife_locate_engine(char *path, size_t bufsz)
{
    const char *env = getenv("RIFE_LIBDIR");
    if (env && *env) {
        snprintf(path, bufsz, "%s/rife512.engine", env);
        if (access(path, F_OK) == 0) return true;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(path, bufsz, "%s/.local/lib/vsr-player/rife512.engine", home);
        if (access(path, F_OK) == 0) return true;
    }
    const char *cwd = getenv("PWD");
    if (cwd && strstr(cwd, "vsr-player")) {
        snprintf(path, bufsz, "%s/third_party/rife/rife512.engine", cwd);
        if (access(path, F_OK) == 0) return true;
    }
    return false;
}

// ── Tile kernels (nvrtc, mirrors yuv_to_rgba.c pattern) ────────────────

static const char *kKernelSrc =
"extern \"C\" __global__ void rife_assemble(\n"
"    const unsigned char* __restrict__ rgba_a, int a_pitch,\n"
"    const unsigned char* __restrict__ rgba_b, int b_pitch,\n"
"    float* __restrict__ in7,\n"
"    int tile_x, int tile_y, int T,\n"
"    int frame_w, int frame_h, float tval)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= T || y >= T) return;\n"
"    int sx = tile_x * T + x; if (sx >= frame_w) sx = frame_w - 1;\n"
"    int sy = tile_y * T + y; if (sy >= frame_h) sy = frame_h - 1;\n"
"    const unsigned char* pa = rgba_a + sy * a_pitch + sx * 4;\n"
"    const unsigned char* pb = rgba_b + sy * b_pitch + sx * 4;\n"
"    size_t k = (size_t)y * T + x;\n"
"    size_t plane = (size_t)T * T;\n"
"    in7[k]            = pa[0] * (1.f/255.f);\n"
"    in7[plane + k]    = pa[1] * (1.f/255.f);\n"
"    in7[2*plane + k]  = pa[2] * (1.f/255.f);\n"
"    in7[3*plane + k]  = pb[0] * (1.f/255.f);\n"
"    in7[4*plane + k]  = pb[1] * (1.f/255.f);\n"
"    in7[5*plane + k]  = pb[2] * (1.f/255.f);\n"
"    in7[6*plane + k]  = tval;\n"
"}\n"
"extern \"C\" __global__ void rife_convert(\n"
"    const float* __restrict__ out3,\n"
"    unsigned char* __restrict__ rgba, int rgba_pitch,\n"
"    int tile_x, int tile_y, int T,\n"
"    int frame_w, int frame_h)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    int fx = tile_x * T + x;\n"
"    int fy = tile_y * T + y;\n"
"    if (fx >= frame_w || fy >= frame_h) return;\n"
"    size_t k = (size_t)y * T + x;\n"
"    size_t plane = (size_t)T * T;\n"
"    unsigned char* o = rgba + fy * rgba_pitch + fx * 4;\n"
"    o[0] = (unsigned char)(__saturatef(out3[k])         * 255.0f + 0.5f);\n"
"    o[1] = (unsigned char)(__saturatef(out3[plane + k]) * 255.0f + 0.5f);\n"
"    o[2] = (unsigned char)(__saturatef(out3[2*plane + k]) * 255.0f + 0.5f);\n"
"    o[3] = 255;\n"
"}\n";

static bool rife_compile_kernels(struct rife_context *c, CUcontext ctx)
{
    if (c->kernels_ready) return true;

    nvrtcProgram prog;
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc, "rife_tiles", 0, NULL, NULL);
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
        cuModuleGetFunction(&c->convert_fn, c->module, "rife_convert") != CUDA_SUCCESS) {
        mp_err(c->log, "rife: cuModuleGetFunction failed\n");
        return false;
    }
    c->kernels_ready = true;
    return true;
}

// ── Lifecycle ──────────────────────────────────────────────────────────

bool rife_init(struct rife_context *c, CUcontext ctx, CUstream stream,
               struct mp_log *log)
{
    memset(c, 0, sizeof(*c));
    c->log = log;

    // engine file (data artifact, shipped alongside the app)
    char path[1024];
    if (!rife_locate_engine(path, sizeof(path))) {
        mp_warn(c->log, "rife: engine file not found (RIFE_LIBDIR / "
                "~/.local/lib/vsr-player / third_party/rife) — passthrough\n");
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
    c->tiles_x = (w + RIFE_TILE - 1) / RIFE_TILE;
    c->tiles_y = (h + RIFE_TILE - 1) / RIFE_TILE;
    c->configured = true;
    (void)stream;
    return true;
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

    const int T = RIFE_TILE;
    bool ok = true;
    for (int ty = 0; ty < c->tiles_y && ok; ty++) {
        for (int tx = 0; tx < c->tiles_x; tx++) {
            float tv = (float)tval;
            void *args_assemble[] = {
                &c->rgba_a, &c->rgba_pitch,
                &c->rgba_b, &c->rgba_pitch,
                &d_in,
                &tx, &ty, (void *)&T,
                &c->w, &c->h, &tv,
            };
            CUresult cr = cuLaunchKernel(c->assemble_fn, 16, 16, 1,
                                         32, 32, 1, 0, stream,
                                         args_assemble, NULL);
            if (cr != CUDA_SUCCESS) {
                mp_err(c->log, "rife: tile %d,%d assemble failed (%d)\n",
                       tx, ty, cr);
                ok = false;
                break;
            }

            if (!rife_engine_run(c->engine, stream)) {
                mp_err(c->log, "rife: tile %d,%d TRT enqueue failed\n", tx, ty);
                ok = false;
                break;
            }

            void *args_convert[] = {
                &d_out,
                &out_rgba, &out_pitch,
                &tx, &ty, (void *)&T,
                &c->w, &c->h,
            };
            cr = cuLaunchKernel(c->convert_fn, 16, 16, 1,
                                32, 32, 1, 0, stream,
                                args_convert, NULL);
            if (cr != CUDA_SUCCESS) {
                mp_err(c->log, "rife: tile %d,%d convert failed (%d)\n",
                       tx, ty, cr);
                ok = false;
                break;
            }
        }
    }
    // Async: enqueue only. The caller synchronizes the stream at its GPU
    // serialization boundary (vf_rife process_pair) — stream-level, never
    // device-level (a device sync would also wait for the VO's stream-0
    // copies and can deadlock — Xid 109 lesson).
    return ok;
}
