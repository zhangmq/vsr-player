// tests/vfg/vfg_timing_probe.c — VFG（NvVFX API）单次 Run 耗时探针
//
// 用途：复现"竖屏/窄幅输入走慢路径"的几何相关耗时（见 README「竖屏视频与插帧质量档」）。
//       纯引擎测量，不经过播放器/mpv；每个 (W,H,MODE) 建一次引擎，warmup 后跑 N 次。
//
// 构建（系统 CUDA 头/lib）：
//   gcc -O2 -o build/scratch/vfg_timing_probe tests/vfg/vfg_timing_probe.c \
//       -I/opt/cuda/include -L/opt/cuda/lib64 -lcuda -ldl
//
// 运行：
//   ./build/scratch/vfg_timing_probe third_party/nvvfx/lib [iters] [W H MODE]
//     iters 默认 30；给了 W H MODE 就只跑这一个配置（MODE: 0=low 1=medium 2=high）。
//   输出每个配置一行：RESULT <W>x<H> mode=<name> ms_per_run=<ms>
//
// 2026-10-21 实测要点（MEDIUM）：1920x1080 2.5 / 1080x1920 8.9 / 1920x1920 1.8 /
// 1920x1921 15.6 / 1920x3328 27-28 / 3840x2160 3.4；竖屏 HIGH 反而更快（8.4）。

// vfg_timing_probe.c — 纯 NvVFX API：VFG 单次 Run 耗时 vs (尺寸, Mode)
// 目的：复现播放器里"竖屏 MEDIUM 最慢（14.9ms）、LOW/HIGH 反而快"的现象，
// 把引擎 Run 与播放器的拷贝/同步剥离开。
// 用法: vfg_timing_probe <libdir> [iters]
// 输出每个 (W,H,mode) 一行 RESULT，ms/run 为整段 CUDA event 平均。
#include <cuda.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct NvCVImage {
    unsigned int width, height;
    int pitch;
    int pixelFormat, componentType;
    unsigned char pixelBytes, componentBytes, numComponents;
    unsigned char planar, gpuMem, colorspace;
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void *);
    unsigned long long bufferBytes;
} NvCVImage;

#define NVCV_RGBA   6
#define NVCV_U8     1
#define NVCV_CHUNKY 0
#define NVCV_GPU    1

typedef int (*PFN_Create)(const char *, void **);
typedef void (*PFN_Destroy)(void *);
typedef int (*PFN_SetU32)(void *, const char *, unsigned int);
typedef int (*PFN_SetImage)(void *, const char *, NvCVImage *);
typedef int (*PFN_SetStream)(void *, const char *, CUstream);
typedef int (*PFN_Load)(void *);
typedef int (*PFN_Run)(void *, int);
typedef int (*PFN_Alloc)(NvCVImage *, unsigned, unsigned, int, int, int, int, unsigned);
typedef int (*PFN_Dealloc)(NvCVImage *);

static PFN_Create   p_create;
static PFN_Destroy  p_destroy;
static PFN_SetU32   p_setu32;
static PFN_SetImage p_setimage;
static PFN_SetStream p_setstream;
static PFN_Load     p_load;
static PFN_Run      p_run;
static PFN_Alloc    p_alloc;
static PFN_Dealloc  p_dealloc;

static void *try_dlopen(const char *dir, const char *name, int global) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return dlopen(path, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
}

static int load_libs(const char *dir) {
    const char *libs[] = {
        "libcudart.so.13", "libnppc.so", "libnppial.so", "libnppicc.so",
        "libnppidei.so", "libnppig.so", "libnppif.so", "libnppim.so",
        "libnppist.so", "libnppitc.so", "libcudnn.so",
        "libNVCVImage.so", "libnvngxruntime.so", "libnvidia-ngx-vsr.so",
        "libVideoFXLocal.so", "libVideoFX.so",
        "libnvVFXVideoSuperRes.so", "libnvVFXVideoFrameGeneration.so",
    };
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
        try_dlopen(dir, libs[i], 1);
    void *vfx = try_dlopen(dir, "libVideoFX.so", 1);
    void *cv  = try_dlopen(dir, "libNVCVImage.so", 0);
    if (!vfx || !cv) { fprintf(stderr, "core libs failed\n"); return 0; }
#define SYM(var, type, lib, name) \
    do { var = (type)dlsym(lib, name); \
         if (!var) { fprintf(stderr, "dlsym(%s) failed: %s\n", name, dlerror()); return 0; } } while (0)
    SYM(p_create, PFN_Create, vfx, "NvVFX_CreateEffect");
    SYM(p_destroy, PFN_Destroy, vfx, "NvVFX_DestroyEffect");
    SYM(p_setu32, PFN_SetU32, vfx, "NvVFX_SetU32");
    SYM(p_setimage, PFN_SetImage, vfx, "NvVFX_SetImage");
    SYM(p_setstream, PFN_SetStream, vfx, "NvVFX_SetCudaStream");
    SYM(p_load, PFN_Load, vfx, "NvVFX_Load");
    SYM(p_run, PFN_Run, vfx, "NvVFX_Run");
    SYM(p_alloc, PFN_Alloc, cv, "NvCVImage_Alloc");
    SYM(p_dealloc, PFN_Dealloc, cv, "NvCVImage_Dealloc");
#undef SYM
    return 1;
}

static double now_ms(void) { return 0; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "third_party/nvvfx/lib";
    int iters = argc > 2 ? atoi(argv[2]) : 30;
    const char *env = getenv("VFG_FLOW_RES");   // 可选：观察 AFGX_FLOW_RESOLUTION 影响
    if (env) setenv("AFGX_FLOW_RESOLUTION", env, 1);

    if (!load_libs(dir)) return 1;
    CUdevice dev; CUcontext ctx; CUstream st;
    if (cuInit(0) != CUDA_SUCCESS) { fprintf(stderr, "cuInit failed\n"); return 1; }
    if (cuDeviceGet(&dev, 0) != CUDA_SUCCESS) { fprintf(stderr, "cuDeviceGet failed\n"); return 1; }
    CUctxCreateParams cp; memset(&cp, 0, sizeof(cp));
    if (cuCtxCreate(&ctx, &cp, 0, dev) != CUDA_SUCCESS) { fprintf(stderr, "cuCtxCreate failed\n"); return 1; }
    cuStreamCreate(&st, CU_STREAM_NON_BLOCKING);

    struct { int w, h; } sizes[] = {
        {1920, 1080}, {1080, 1920}, {1920, 3328}, {3328, 1920}, {3840, 2160},
    };
    int only_w = argc > 5 ? atoi(argv[3]) : 0;
    int only_h = argc > 5 ? atoi(argv[4]) : 0;
    int only_mode = argc > 5 ? atoi(argv[5]) : -1;
    if (only_w > 0 && only_h > 0) {
        sizes[0].w = only_w;
        sizes[0].h = only_h;
    }
    unsigned nsizes = only_w > 0 ? 1u : (unsigned)(sizeof(sizes)/sizeof(sizes[0]));
    const char *mode_name[] = {"low", "medium", "high"};

    for (unsigned s = 0; s < nsizes; s++) {
        int W = sizes[s].w, H = sizes[s].h;
        if (only_w && (W != only_w || H != only_h)) continue;
        for (int mode = 0; mode <= 2; mode++) {
            if (only_mode >= 0 && mode != only_mode) continue;
            void *h = NULL;
            if (p_create("VideoFrameGeneration", &h) != 0 || !h) continue;
            p_setu32(h, "InputWidth", (unsigned)W);
            p_setu32(h, "InputHeight", (unsigned)H);
            p_setu32(h, "Mode", (unsigned)mode);
            p_setu32(h, "FrameMultiplier", 2);
            p_setu32(h, "FrameIndex", 1);
            p_setu32(h, "AutomaticShotChangeDetectionEnabled", 0);
            NvCVImage i0, i1, o;
            memset(&i0, 0, sizeof(i0)); memset(&i1, 0, sizeof(i1)); memset(&o, 0, sizeof(o));
            int ar = 0;
            ar |= p_alloc(&i0, W, H, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32);
            ar |= p_alloc(&i1, W, H, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32);
            ar |= p_alloc(&o,  W, H, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32);
            if (ar) { fprintf(stderr, "alloc failed %dx%d mode=%d\n", W, H, mode); return 1; }
            p_setimage(h, "SrcImage0", &i0);
            p_setimage(h, "SrcImage1", &i1);
            p_setimage(h, "DstImage0", &o);
            p_setstream(h, "CudaStream", st);
            if (p_load(h) != 0) { fprintf(stderr, "load failed %dx%d\n", W, H); return 1; }

            // 填两张输入（内容无关紧要，统一灰色即可）
            size_t bytes = (size_t)i0.pitch * H;
            void *fill = malloc(bytes);
            memset(fill, 128, bytes);
            cuMemcpyHtoDAsync((CUdeviceptr)i0.pixels, fill, bytes, st);
            cuMemcpyHtoDAsync((CUdeviceptr)i1.pixels, fill, bytes, st);
            cuStreamSynchronize(st);

            for (int i = 0; i < 5; i++) p_run(h, 0);   // warmup
            cuStreamSynchronize(st);

            CUevent e0, e1;
            cuEventCreate(&e0, 0); cuEventCreate(&e1, 0);
            cuEventRecord(e0, st);
            for (int i = 0; i < iters; i++) p_run(h, 0);
            cuEventRecord(e1, st);
            cuEventSynchronize(e1);
            float ms = 0; cuEventElapsedTime(&ms, e0, e1);
            printf("RESULT %dx%d mode=%s ms_per_run=%.2f\n",
                   W, H, mode_name[mode], ms / iters);

            cuEventDestroy(e0); cuEventDestroy(e1);
            free(fill);
            p_dealloc(&i0); p_dealloc(&i1); p_dealloc(&o);
            p_destroy(h);
        }
    }
    (void)now_ms;
    return 0;
}
