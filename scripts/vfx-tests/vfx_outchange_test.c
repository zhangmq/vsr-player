/*
 * vfx_outchange_test.c — 验证"输入不变、仅输出尺寸变化"是否需要重建引擎
 *
 * 背景：全屏切换时视频输入不变（1280x720），仅 auto 倍率变化导致输出
 * 尺寸变化（如 2x→3x）。现行代码"尺寸变化必须重建"的结论来自 vfx_test5
 * （SetImage 换输入/输出尺寸 → 输出错乱 PSNR 7.4dB）——但该实验未隔离
 * 变量。本实验：引擎 A 从 720p→1440p 加载后，仅 SetImage(OUT) 换成
 * 2160p 缓冲继续 Run；对照组引擎 B 直接加载 720p→2160p。同输入帧下
 * 对比两引擎输出 PSNR：
 *   PSNR > 40dB  → "仅输出变更"可行（SetImage 足够，无需重建）
 *   PSNR 低      → 确认 vfx_test5 结论，仅输出变更也必须重建
 *
 * 用法：cd vsr-player && gcc ... && ./vfx_outchange_test
 */
#include <cuda.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

#define NVCV_RGBA 6
#define NVCV_U8 1
#define NVCV_CHUNKY 0
#define NVCV_GPU 1

typedef int (*PFN_CreateEffect)(const char*, void**);
typedef void (*PFN_DestroyEffect)(void*);
typedef int (*PFN_SetU32)(void*, const char*, unsigned int);
typedef int (*PFN_SetImage)(void*, const char*, NvCVImage*);
typedef int (*PFN_GetImage)(void*, const char*, NvCVImage*);
typedef int (*PFN_SetCudaStream)(void*, const char*, CUstream);
typedef int (*PFN_Load)(void*);
typedef int (*PFN_Run)(void*, int);
typedef int (*PFN_Alloc)(NvCVImage*, unsigned int, unsigned int, int, int, int, int, unsigned int);
typedef int (*PFN_Dealloc)(NvCVImage*);

static PFN_CreateEffect  pfn_CreateEffect;
static PFN_DestroyEffect pfn_DestroyEffect;
static PFN_SetU32        pfn_SetU32;
static PFN_SetImage      pfn_SetImage;
static PFN_GetImage      pfn_GetImage;
static PFN_SetCudaStream pfn_SetCudaStream;
static PFN_Load          pfn_Load;
static PFN_Run           pfn_Run;
static PFN_Alloc         pfn_Alloc;
static PFN_Dealloc       pfn_Dealloc;

static int load_libs(void)
{
    const char *libs[] = {
        "libnppc.so", "libnppial.so", "libnppicc.so", "libnppidei.so",
        "libnppig.so", "libnppif.so", "libnppim.so", "libnppist.so",
        "libnppitc.so", "libcudnn.so",
        "libnvinfer.so", "libnvinfer_plugin.so", "libnvonnxparser.so",
        "libNVCVImage.so", "libnvngxruntime.so", "libnvidia-ngx-vsr.so",
        "libVideoFXLocal.so", "libVideoFX.so", "libnvVFXVideoSuperRes.so",
    };
    for (int i = 0; i < (int)(sizeof(libs)/sizeof(libs[0])); i++) {
        void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) fprintf(stderr, "dlopen(%s): %s\n", libs[i], dlerror());
    }
    void *vfx = dlopen("libVideoFX.so", RTLD_NOW | RTLD_GLOBAL);
    void *nvcv = dlopen("libNVCVImage.so", RTLD_NOW);
    if (!vfx || !nvcv) { fprintf(stderr, "core libs failed\n"); return -1; }
    pfn_CreateEffect  = dlsym(vfx, "NvVFX_CreateEffect");
    pfn_DestroyEffect = dlsym(vfx, "NvVFX_DestroyEffect");
    pfn_SetU32        = dlsym(vfx, "NvVFX_SetU32");
    pfn_SetImage      = dlsym(vfx, "NvVFX_SetImage");
    pfn_GetImage      = dlsym(vfx, "NvVFX_GetImage");
    pfn_SetCudaStream = dlsym(vfx, "NvVFX_SetCudaStream");
    pfn_Load          = dlsym(vfx, "NvVFX_Load");
    pfn_Run           = dlsym(vfx, "NvVFX_Run");
    pfn_Alloc         = dlsym(nvcv, "NvCVImage_Alloc");
    pfn_Dealloc       = dlsym(nvcv, "NvCVImage_Dealloc");
    if (!pfn_CreateEffect || !pfn_DestroyEffect || !pfn_SetU32 ||
        !pfn_SetImage || !pfn_GetImage || !pfn_SetCudaStream ||
        !pfn_Load || !pfn_Run || !pfn_Alloc || !pfn_Dealloc) {
        fprintf(stderr, "dlsym incomplete\n"); return -1;
    }
    return 0;
}

static int alloc_img(NvCVImage *img, int w, int h)
{
    return pfn_Alloc(img, (unsigned)w, (unsigned)h, NVCV_RGBA, NVCV_U8,
                     NVCV_CHUNKY, NVCV_GPU, 32);
}

static int psnr(const unsigned char *a, const unsigned char *b,
                size_t n)
{
    double mse = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse <= 1e-9) return 999;
    return (int)lrint(10.0 * log10(255.0 * 255.0 / mse));
}

int main(void)
{
    if (load_libs() != 0) return 1;
    cuInit(0);
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx; cuCtxCreate(&ctx, CU_CTX_SCHED_AUTO, dev, 0);
    CUstream stream; cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);

    const int IN_W = 1280, IN_H = 720;
    const int OUT2_W = 2560, OUT2_H = 1440;   // 引擎 A 加载尺寸（2x）
    const int OUT3_W = 3840, OUT3_H = 2160;   // 引擎 A 换缓冲目标 + 引擎 B（3x）

    // ── 输入缓冲（A/B 共用同一输入内容）──────────────────────────────
    NvCVImage in_img = {0};
    alloc_img(&in_img, IN_W, IN_H);
    size_t in_bytes = (size_t)in_img.pitch * IN_H;
    unsigned char *host_in = malloc(in_bytes);
    unsigned int seed = 42;
    for (size_t i = 0; i < in_bytes; i += 4) {
        seed = seed * 1103515245 + 12345;
        host_in[i+0] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        host_in[i+1] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        host_in[i+2] = (unsigned char)((seed >> 16) & 0xFF);
        host_in[i+3] = 255;
    }
    cuMemcpyHtoD((CUdeviceptr)in_img.pixels, host_in, in_bytes);
    free(host_in);

    // ── 引擎 A：720p→1440p 加载，之后 SetImage(OUT) 换成 2160p ───────
    void *hA = NULL;
    pfn_CreateEffect("VideoSuperRes", &hA);
    pfn_SetU32(hA, "QualityLevel", 3);
    NvCVImage a_out2 = {0}; alloc_img(&a_out2, OUT2_W, OUT2_H);
    pfn_SetImage(hA, "DstImage0", &a_out2);
    pfn_SetImage(hA, "SrcImage0", &in_img);
    pfn_SetCudaStream(hA, "CudaStream", stream);
    if (pfn_Load(hA) != 0) { fprintf(stderr, "A Load failed\n"); return 1; }
    printf("[A] loaded 1280x720 -> 2560x1440\n");

    // 换输出缓冲到 2160p（不重建）
    NvCVImage a_out3 = {0}; alloc_img(&a_out3, OUT3_W, OUT3_H);
    int sr = pfn_SetImage(hA, "DstImage0", &a_out3);
    printf("[A] SetImage(DstImage0) to 3840x2160 -> %d\n", sr);

    // Run 3 帧（warmup + 稳定）
    for (int i = 0; i < 3; i++) {
        int rr = pfn_Run(hA, 0);
        if (rr != 0) { fprintf(stderr, "A Run[%d] -> %d\n", i, rr); return 1; }
    }
    cuStreamSynchronize(stream);

    NvCVImage a_got = {0};
    pfn_GetImage(hA, "DstImage0", &a_got);
    printf("[A] after SetImage: out %ux%u pitch=%d (expected %dx%d)\n",
           a_got.width, a_got.height, a_got.pitch, OUT3_W, OUT3_H);

    // ── 引擎 B（对照组）：直接加载 720p→2160p ─────────────────────────
    void *hB = NULL;
    pfn_CreateEffect("VideoSuperRes", &hB);
    pfn_SetU32(hB, "QualityLevel", 3);
    NvCVImage b_out3 = {0}; alloc_img(&b_out3, OUT3_W, OUT3_H);
    pfn_SetImage(hB, "DstImage0", &b_out3);
    pfn_SetImage(hB, "SrcImage0", &in_img);
    pfn_SetCudaStream(hB, "CudaStream", stream);
    if (pfn_Load(hB) != 0) { fprintf(stderr, "B Load failed\n"); return 1; }
    for (int i = 0; i < 3; i++) {
        int rr = pfn_Run(hB, 0);
        if (rr != 0) { fprintf(stderr, "B Run[%d] -> %d\n", i, rr); return 1; }
    }
    cuStreamSynchronize(stream);
    printf("[B] loaded 1280x720 -> 3840x2160 directly\n");

    // ── 对比：A（换缓冲）vs B（直接）同输入输出 ──────────────────────
    size_t out_bytes = (size_t)OUT3_W * OUT3_H * 4;
    unsigned char *ha = malloc(out_bytes);
    unsigned char *hb = malloc(out_bytes);
    CUDA_MEMCPY2D cp = {0};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST;
    cp.dstPitch = (size_t)OUT3_W * 4;
    cp.WidthInBytes = (size_t)OUT3_W * 4;
    cp.Height = OUT3_H;

    cp.srcDevice = (CUdeviceptr)a_got.pixels;
    cp.srcPitch = (size_t)a_got.pitch;
    cp.dstHost = ha;
    cuMemcpy2D(&cp);
    cp.srcDevice = (CUdeviceptr)b_out3.pixels;
    cp.srcPitch = (size_t)b_out3.pitch;
    cp.dstHost = hb;
    cuMemcpy2D(&cp);

    // 像素差异统计（跳过首行/首列——tile 边界特征也纳入比较）
    int same = 0, diff = 0;
    size_t total = out_bytes;
    for (size_t i = 0; i < total; i += 4)
        if (ha[i] == hb[i] && ha[i+1] == hb[i+1] && ha[i+2] == hb[i+2]) same++;
        else diff++;
    printf("[RESULT] PSNR(A-vs-B) = %d dB  (%d identical px / %d diff px of %zu)\n",
           psnr(ha, hb, total), same, diff, total / 4);

    printf("[VERDICT] %s\n", psnr(ha, hb, total) >= 40
        ? "SetImage 仅换输出可行——全屏切换无需重建引擎"
        : "SetImage 仅换输出仍错乱——vfx_test5 结论成立，输出变更必须重建");

    // ── 清理 ──────────────────────────────────────────────────────────
    pfn_DestroyEffect(hA);
    pfn_DestroyEffect(hB);
    pfn_Dealloc(&in_img); pfn_Dealloc(&a_out2); pfn_Dealloc(&a_out3);
    pfn_Dealloc(&b_out3);
    cuStreamDestroy(stream);
    cuCtxDestroy(ctx);
    return 0;
}
