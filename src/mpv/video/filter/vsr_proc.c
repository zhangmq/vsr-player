#include "vsr_internal.h"

#include <cuda.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Types from nvCVImage.h / nvVideoEffects.h (C-compatible) ────────────

typedef struct NvCVImage {
    unsigned int width, height;
    int pitch;
    int pixelFormat, componentType;
    unsigned char pixelBytes, componentBytes, numComponents;
    unsigned char planar, gpuMem, colorspace;
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void*);
    unsigned long long bufferBytes;
} NvCVImage;

#define NVCV_RGB    4
#define NVCV_RGBA   6
#define NVCV_U8     1
#define NVCV_F32    7
#define NVCV_PLANAR 1
#define NVCV_CHUNKY 0
#define NVCV_GPU    1

#define NVVFX_INPUT_IMAGE   "SrcImage0"
#define NVVFX_OUTPUT_IMAGE  "DstImage0"
#define NVVFX_CUDA_STREAM   "CudaStream"

// ── NvVFX API function pointer types ─────────────────────────────────────

typedef int (*PFN_NvVFX_CreateEffect)(const char*, void**);
typedef void (*PFN_NvVFX_DestroyEffect)(void*);
typedef int (*PFN_NvVFX_SetU32)(void*, const char*, unsigned int);
typedef int (*PFN_NvVFX_SetImage)(void*, const char*, NvCVImage*);
typedef int (*PFN_NvVFX_GetImage)(void*, const char*, NvCVImage*);
typedef int (*PFN_NvVFX_SetCudaStream)(void*, const char*, CUstream);
typedef int (*PFN_NvVFX_Load)(void*);
typedef int (*PFN_NvVFX_Run)(void*, int);
typedef int (*PFN_NvCVImage_Alloc)(NvCVImage*, unsigned int, unsigned int,
    int, int, int, int, unsigned int);
typedef int (*PFN_NvCVImage_Dealloc)(NvCVImage*);
typedef int (*PFN_NvCVImage_Transfer)(const NvCVImage*, NvCVImage*, float,
    CUstream, NvCVImage*);

// ── Static globals ───────────────────────────────────────────────────────

static void *g_vfx_lib = NULL;
static void *g_nvcv_lib = NULL;

static PFN_NvVFX_CreateEffect    pfn_NvVFX_CreateEffect;
static PFN_NvVFX_DestroyEffect   pfn_NvVFX_DestroyEffect;
static PFN_NvVFX_SetU32          pfn_NvVFX_SetU32;
static PFN_NvVFX_SetImage        pfn_NvVFX_SetImage;
static PFN_NvVFX_GetImage        pfn_NvVFX_GetImage;
static PFN_NvVFX_SetCudaStream   pfn_NvVFX_SetCudaStream;
static PFN_NvVFX_Load            pfn_NvVFX_Load;
static PFN_NvVFX_Run             pfn_NvVFX_Run;
static PFN_NvCVImage_Alloc       pfn_NvCVImage_Alloc;
static PFN_NvCVImage_Dealloc     pfn_NvCVImage_Dealloc;
static PFN_NvCVImage_Transfer    pfn_NvCVImage_Transfer;

// Internal NvCVImage descriptors for alloc/dealloc lifecycle tracking
static NvCVImage g_in_img;
static NvCVImage g_out_img;
static NvCVImage g_tmp_img;

// ── Library loading ──────────────────────────────────────────────────────

#define LOAD_SYM(LIB, NAME)                                      \
    do {                                                          \
        pfn_##NAME = (PFN_##NAME)dlsym(LIB, #NAME);             \
        if (!pfn_##NAME) {                                        \
            fprintf(stderr, "VSR: dlsym(" #NAME ") failed: %s\n", \
                    dlerror());                                   \
            return false;                                         \
        }                                                         \
    } while (0)

static bool load_nvvfx_libraries(void) {
    if (g_vfx_lib) return true;

    // Find nvvfx lib directory — try common locations
    const char *home = getenv("HOME");
    char homedir_path[1024] = "";
    if (home) {
        snprintf(homedir_path, sizeof(homedir_path), "%s/.local/lib/vsr-player/", home);
    }
    const char *search[] = {
#ifdef VSR_INSTALL_LIBDIR
        VSR_INSTALL_LIBDIR,           // compile-time override (packaging)
#endif
        homedir_path,                  // user-local default
        "third_party/nvvfx/lib/",      // dev: run from project root
        "build/lib/",
        "",
    };
    const char *nvvfx_dir = NULL;
    int n_search = sizeof(search) / sizeof(search[0]);
    for (int i = 0; i < n_search; i++) {
        if (!search[i] || !search[i][0]) continue;       // skip NULL or empty
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", search[i], "libNVCVImage.so");
        if (access(path, R_OK) == 0) { nvvfx_dir = search[i]; break; }
    }
    if (!nvvfx_dir) {
        fprintf(stderr, "VSR: nvvfx libs not found\n");
        return false;
    }

    // Load all nvvfx .so files (order matters for deps)
    // Topological order: deps before dependents
    const char *libs[] = {
        "libnppc.so", "libnppial.so", "libnppicc.so", "libnppidei.so",
        "libnppig.so", "libnppif.so", "libnppim.so", "libnppist.so",
        "libnppitc.so", "libcudnn.so",
        "libnvinfer.so", "libnvinfer_plugin.so", "libnvonnxparser.so",
        "libNVCVImage.so",    // before libnvngxruntime which needs it
        "libnvngxruntime.so", "libnvidia-ngx-vsr.so",
        "libVideoFXLocal.so", "libVideoFX.so",
        "libnvVFXVideoSuperRes.so",
    };
    int nlibs = sizeof(libs) / sizeof(libs[0]);
    for (int i = 0; i < nlibs; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", nvvfx_dir, libs[i]);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "VSR: dlopen(%s): %s\n", libs[i], dlerror());
            // Continue — some libs might be missing but others work
        }
    }

    // Retrieve handles for the two we need symbols from
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", nvvfx_dir, "libVideoFX.so");
    g_vfx_lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    snprintf(path, sizeof(path), "%s%s", nvvfx_dir, "libNVCVImage.so");
    g_nvcv_lib = dlopen(path, RTLD_NOW);
    if (!g_vfx_lib || !g_nvcv_lib) {
        fprintf(stderr, "VSR: core libs failed\n");
        if (g_vfx_lib) dlclose(g_vfx_lib);
        if (g_nvcv_lib) dlclose(g_nvcv_lib);
        g_vfx_lib = NULL;
        g_nvcv_lib = NULL;
        return false;
    }

    LOAD_SYM(g_vfx_lib,  NvVFX_CreateEffect);
    LOAD_SYM(g_vfx_lib,  NvVFX_DestroyEffect);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetU32);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetImage);
    LOAD_SYM(g_vfx_lib,  NvVFX_GetImage);
    LOAD_SYM(g_vfx_lib,  NvVFX_SetCudaStream);
    LOAD_SYM(g_vfx_lib,  NvVFX_Load);
    LOAD_SYM(g_vfx_lib,  NvVFX_Run);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Alloc);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Dealloc);
    LOAD_SYM(g_nvcv_lib, NvCVImage_Transfer);

    fprintf(stderr, "VSR: NvVFX libraries loaded\n");
    return true;
}

#undef LOAD_SYM

// ── 图像缓冲按需分配（尺寸变化时重建，旧缓冲先释放）──────────────────
// 单一分配入口：vsr_init 与 vsr_set_output/set_input 共用，杜绝
// "旧缓冲未释放即被覆盖"的泄漏路径（旧实现 scale 热更新会 memset
// 丢弃 g_out_img 而未 Dealloc）。
static bool ensure_img(NvCVImage *img, int w, int h, const char *tag) {
    if (img->pixels && img->width == (unsigned)w && img->height == (unsigned)h)
        return true;
    if (img->pixels) {
        pfn_NvCVImage_Dealloc(img);
        memset(img, 0, sizeof(*img));
    }
    int ar = pfn_NvCVImage_Alloc(img, (unsigned)w, (unsigned)h,
                                 NVCV_RGBA, NVCV_U8,
                                 NVCV_CHUNKY, NVCV_GPU, 32);
    if (ar != 0) {
        fprintf(stderr, "VSR: Alloc(%s %dx%d) failed (%d)\n", tag, w, h, ar);
        memset(img, 0, sizeof(*img));
        return false;
    }
    return true;
}

// ── vsr_init ─────────────────────────────────────────────────────────────

bool vsr_init(struct vsr_context *c, int in_w, int in_h,
              int out_w, int out_h, int quality, CUstream stream) {
    if (!load_nvvfx_libraries()) return false;

    memset(c, 0, sizeof(*c));
    c->in_w = in_w;
    c->in_h = in_h;
    c->out_w = out_w;
    c->out_h = out_h;
    c->quality = quality;

    // Create effect — responds to "VideoSuperRes" (not "SuperRes")
    int ret = pfn_NvVFX_CreateEffect("VideoSuperRes", &c->handle);
    if (ret != 0) {
        fprintf(stderr, "VSR: CreateEffect(VideoSuperRes) failed (%d)\n", ret);
        return false;
    }

    // QualityLevel (not "Strength")
    pfn_NvVFX_SetU32(c->handle, "QualityLevel", (unsigned int)quality);
    {
        bool denoise = (in_w == out_w && in_h == out_h);
        fprintf(stderr, "VSR: [%s] %dx%d->%dx%d quality=%d %s\n",
                denoise ? "DENOISE" : "UPSCALE",
                in_w, in_h, out_w, out_h, quality,
                denoise ? "(1:1 denoising)" : "(AI super-resolution)");
    }

    // ── Output image: RGBA U8 chunky GPU ──────────────────────────────────
    if (!ensure_img(&g_out_img, out_w, out_h, "output")) {
        vsr_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, NVVFX_OUTPUT_IMAGE, &g_out_img);
    fprintf(stderr, "VSR: DstImage0 RGBA U8 -> %d\n", ret);
    if (ret != 0) {
        vsr_destroy(c);
        return false;
    }
    c->out_pixels = g_out_img.pixels;
    c->out_pitch = (int)g_out_img.pitch;

    // ── Input image: RGBA U8 chunky GPU ───────────────────────────────────
    if (!ensure_img(&g_in_img, in_w, in_h, "input")) {
        vsr_destroy(c);
        return false;
    }
    ret = pfn_NvVFX_SetImage(c->handle, NVVFX_INPUT_IMAGE, &g_in_img);
    if (ret != 0) {
        fprintf(stderr, "VSR: SetImage(input RGBA U8) failed (%d)\n", ret);
        vsr_destroy(c);
        return false;
    }
    c->in_pixels = g_in_img.pixels;
    c->in_pitch = (int)g_in_img.pitch;

    // ── Temp buffer for NvCVImage_Transfer ────────────────────────────────
    {
        int max_dim = (out_w > in_w) ? out_w : in_w;
        int max_h = (out_h > in_h) ? out_h : in_h;
        if (!ensure_img(&g_tmp_img, max_dim, max_h, "temp"))
            memset(&g_tmp_img, 0, sizeof(g_tmp_img));   // 非致命
    }
    c->tmp_pixels = g_tmp_img.pixels;
    c->tmp_pitch = (int)g_tmp_img.pitch;

    // ── CUDA stream ───────────────────────────────────────────────────────
    if (stream) {
        c->stream = stream;
        c->own_stream = false;
    } else {
        cuStreamCreate(&c->stream, CU_STREAM_NON_BLOCKING);
        c->own_stream = true;
    }
    pfn_NvVFX_SetCudaStream(c->handle, NVVFX_CUDA_STREAM, c->stream);

    // ── Load model ────────────────────────────────────────────────────────
    ret = pfn_NvVFX_Load(c->handle);
    if (ret != 0) {
        fprintf(stderr, "VSR: Load failed (%d)\n", ret);
        vsr_destroy(c);
        return false;
    }

    c->ready = true;
    fprintf(stderr, "VSR: init ok\n");
    return true;
}

// ── vsr_warmup ────────────────────────────────────────────────────────────
// Must be called after vsr_init, before the first vsr_process.
// Blocks until complete (~30ms for 3 random frames on the given stream).

bool vsr_warmup(struct vsr_context *c, CUstream stream)
{
    // Temporarily set stream for warmup, restore after
    CUstream saved = c->stream;
    c->stream = stream;

    size_t in_bytes = (size_t)c->in_pitch * c->in_h;
    unsigned char *random_rgba = (unsigned char*)malloc(in_bytes);
    if (!random_rgba) {
        fprintf(stderr, "VSR: warmup malloc failed\n");
        c->stream = saved;
        return false;
    }
    unsigned int seed = 42;
    for (size_t i = 0; i < in_bytes; i += 4) {
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 0] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 1] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        random_rgba[i + 2] = (unsigned char)((seed >> 16) & 0xFF);
        random_rgba[i + 3] = 255;
    }
    cuMemcpyHtoD((CUdeviceptr)c->in_pixels, random_rgba, in_bytes);
    free(random_rgba);

    for (int wu = 0; wu < 3; wu++) {
        int wr = pfn_NvVFX_Run(c->handle, 0);
        if (wr != 0) {
            fprintf(stderr, "VSR: warmup[%d] Run -> %d\n", wu, wr);
            c->stream = saved;
            return false;
        }
    }
    c->stream = saved;
    fprintf(stderr, "VSR: warmup complete (3 frames with random data)\n");
    return true;
}

// ── vsr_process ──────────────────────────────────────────────────────────
// Caller must have written RGBA into c->in_pixels (HtoD or D2D on stream).
// Runs VSR synchronously, returns output descriptor from pre-allocated buffer.

bool vsr_process(struct vsr_context *c, CUstream stream,
                 void **output_ptr, int *out_w, int *out_h, int *out_pitch) {
    if (!c->ready) return false;

    int ret = pfn_NvVFX_Run(c->handle, 0);
    if (ret != 0) {
        fprintf(stderr, "VSR: Run failed (%d)\n", ret);
        return false;
    }

    ret = pfn_NvVFX_GetImage(c->handle, NVVFX_OUTPUT_IMAGE, &g_out_img);
    if (ret != 0) {
        fprintf(stderr, "VSR: GetImage(output) failed (%d)\n", ret);
        return false;
    }

    *output_ptr = g_out_img.pixels;
    *out_w = (int)g_out_img.width;
    *out_h = (int)g_out_img.height;
    if (out_pitch) *out_pitch = (int)g_out_img.pitch;

    return true;
}

// ── 轻量热更新（引擎常驻）──────────────────────────────────────────────
// SDK 实验结论（vfx_test5）：
//   - SetImage 换输入/输出尺寸 → 输出错乱（PSNR 7.4dB）——引擎内部
//     tile 切分与 Load 时尺寸绑定，SetImage 仅更新元数据。尺寸变化
//     必须重建管线（vsr_destroy + vsr_init）。
//   - SetU32 改 QualityLevel → 生效（q1 vs q3 输出 PSNR 44.8dB 结构化
//     差异）——quality 可轻量更新，引擎常驻。
// 旧实现每次热更新全量重建引擎（~1s Load 延迟 + 销毁竞态 + scale 路径
// 旧引擎泄漏）；上一版轻量实现（SetImage 换尺寸）输出错乱——均废弃。

bool vsr_set_quality(struct vsr_context *c, int quality) {
    if (!c->handle || !c->ready) return false;
    int ret = pfn_NvVFX_SetU32(c->handle, "QualityLevel", (unsigned)quality);
    if (ret != 0) {
        fprintf(stderr, "VSR: SetU32(QualityLevel=%d) failed (%d)\n",
                quality, ret);
        return false;
    }
    c->quality = quality;
    fprintf(stderr, "VSR: quality -> %d (engine kept)\n", quality);
    return true;
}

// ── vsr_destroy ──────────────────────────────────────────────────────────
// 销毁前等待在途工作完成：Run 为异步提交，在途推理若仍在 GPU 上执行，
// Dealloc/DestroyEffect 释放其目标缓冲会导致 CUDA 非法访问（ILLEGAL_ADDRESS
// / GPU hang）。
//
// 同步范围 = 自己的流（c->stream，VSR 推理所在流，含 SDK 内部流——
// NvVFX_SetCudaStream 指定的流；推理完成即内部流空闲）。不能做设备级
// 同步（cuCtxSynchronize）：它会把 VO map 的 stream 0（GUI 渲染线程的
// 异步拷贝）也卷进来等——渲染循环随 mpv 核心阻塞而停止后，stream 0 的
// 拷贝永远不完成 → ctx sync 死锁（Xid 109 场景 core 实测：destroy 卡在
// cuCtxSynchronize，stream 0 NOT_READY）。调用方须已 cuCtxPushCurrent。

void vsr_destroy(struct vsr_context *c) {
    if (c->stream)
        cuStreamSynchronize(c->stream);
    // 再等 VO map 流（stream 0）完成：SDK DestroyEffect（NGX
    // ReleaseBuffers）内部等待"全部 CUDA 流空闲"——stream 0 上有 GUI
    // 渲染的 map 拷贝在途时（map 的等待已由 ext_wait 的 flush 满足，
    // 拷贝无依赖会完成），不等就进入 DestroyEffect → SDK 等流空闲卡死
    //（Xid 109 CTX SWITCH TIMEOUT，core 实测卡在 ReleaseBuffers）。
    // 注意：只能流级同步，不能 cuCtxSynchronize——device 级会把渲染
    // 循环持续提交的 map 也等进来，GPU 永不静止 → 同样死锁。
    cuStreamSynchronize(0);
    if (c->handle) {
        pfn_NvVFX_DestroyEffect(c->handle);
        c->handle = NULL;
    }
    if (g_out_img.pixels) {
        pfn_NvCVImage_Dealloc(&g_out_img);
        memset(&g_out_img, 0, sizeof(g_out_img));
    }
    if (g_in_img.pixels) {
        pfn_NvCVImage_Dealloc(&g_in_img);
        memset(&g_in_img, 0, sizeof(g_in_img));
    }
    if (g_tmp_img.pixels) {
        pfn_NvCVImage_Dealloc(&g_tmp_img);
        memset(&g_tmp_img, 0, sizeof(g_tmp_img));
    }
    if (c->own_stream && c->stream) {
        cuStreamDestroy(c->stream);
        c->stream = NULL;
        c->own_stream = false;
    }
    c->ready = false;
    c->in_w = c->in_h = c->out_w = c->out_h = 0;
}
