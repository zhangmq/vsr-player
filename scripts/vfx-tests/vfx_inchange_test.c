/*
 * vfx_inchange_test.c — 验证"输入尺寸变化"是否需要重建引擎
 *
 * 背景：vsr_set_output 已验证"仅输出变化无需重建"（PSNR 999dB 逐字节
 * 一致）。本实验验证输入变化——变体矩阵：
 *
 *   A2  仅输入变：引擎 Load(720p→1440p) 后 SetImage(IN) 换 1080p 缓冲，
 *       输出不变（1440p）→ 对照引擎直接 Load(1080p→1440p)
 *   A1  输入+输出变：A2 基础上再 SetImage(OUT) 换 2160p → 对照引擎
 *       直接 Load(1080p→2160p)（vfx_test5 "输入+输出同时换"场景复验）
 *   A3  缩小方向：引擎 Load(1080p→2160p) 后 SetImage(IN→720p) +
 *       SetImage(OUT→1440p) → 对照引擎直接 Load(720p→1440p)
 *
 * 同输入帧下实验组 vs 对照组 PSNR：
 *   ≥40dB  → 该变体无需重建（SetImage 足够）
 *   低     → 输入变化必须重建（tile 切分绑定 Load 时输入尺寸）
 *
 * 用法：cd vsr-player && gcc ... && ./vfx_inchange_test
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

// 填随机 RGBA 到 GPU 缓冲
static void fill_random(NvCVImage *img, unsigned int seed)
{
    size_t in_bytes = (size_t)img->pitch * img->height;
    unsigned char *host = malloc(in_bytes);
    for (size_t i = 0; i < in_bytes; i += 4) {
        seed = seed * 1103515245 + 12345;
        host[i+0] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        host[i+1] = (unsigned char)((seed >> 16) & 0xFF);
        seed = seed * 1103515245 + 12345;
        host[i+2] = (unsigned char)((seed >> 16) & 0xFF);
        host[i+3] = 255;
    }
    cuMemcpyHtoD((CUdeviceptr)img->pixels, host, in_bytes);
    free(host);
}

// 创建并 Load 引擎
static void *make_engine(int in_w, int in_h, int out_w, int out_h,
                         NvCVImage *in_img, NvCVImage *out_img,
                         CUstream stream)
{
    void *h = NULL;
    pfn_CreateEffect("VideoSuperRes", &h);
    pfn_SetU32(h, "QualityLevel", 3);
    pfn_SetImage(h, "DstImage0", out_img);
    pfn_SetImage(h, "SrcImage0", in_img);
    pfn_SetCudaStream(h, "CudaStream", stream);
    if (pfn_Load(h) != 0) { fprintf(stderr, "Load(%dx%d->%dx%d) failed\n",
                                    in_w, in_h, out_w, out_h); return NULL; }
    return h;
}

// 下载 GPU 输出到 host（RGBA 像素）
static void dump_out(void *handle, int w, int h, unsigned char *host)
{
    NvCVImage got = {0};
    pfn_GetImage(handle, "DstImage0", &got);
    CUDA_MEMCPY2D cp = {0};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = (CUdeviceptr)got.pixels;
    cp.srcPitch = (size_t)got.pitch;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST;
    cp.dstHost = host;
    cp.dstPitch = (size_t)w * 4;
    cp.WidthInBytes = (size_t)w * 4;
    cp.Height = (size_t)h;
    cuMemcpy2D(&cp);
}

int main(void)
{
    if (load_libs() != 0) return 1;
    cuInit(0);
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx; cuCtxCreate(&ctx, CU_CTX_SCHED_AUTO, dev, 0);
    CUstream stream; cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);

    // 输入缓冲：720p 与 1080p（不同随机内容）
    NvCVImage in720 = {0}; alloc_img(&in720, 1280, 720);
    NvCVImage in1080 = {0}; alloc_img(&in1080, 1920, 1080);
    fill_random(&in720, 7);
    fill_random(&in1080, 99);

    // 输出缓冲
    NvCVImage out1440 = {0}; alloc_img(&out1440, 2560, 1440);
    NvCVImage out2160 = {0}; alloc_img(&out2160, 3840, 2160);

    // ── A2：仅输入变（720p→1080p），输出不变（1440p）────────────────
    void *hA = make_engine(1280, 720, 2560, 1440, &in720, &out1440, stream);
    if (!hA) return 1;
    printf("[A2] engine loaded 720p->1440p\n");

    int sr = pfn_SetImage(hA, "SrcImage0", &in1080);
    printf("[A2] SetImage(SrcImage0) to 1080p -> %d\n", sr);
    for (int i = 0; i < 3; i++) {
        int rr = pfn_Run(hA, 0);
        if (rr != 0) { fprintf(stderr, "A2 Run[%d] -> %d\n", i, rr); return 1; }
    }
    cuStreamSynchronize(stream);

    void *hA2ref = make_engine(1920, 1080, 2560, 1440, &in1080, &out1440, stream);
    if (!hA2ref) return 1;
    for (int i = 0; i < 3; i++) pfn_Run(hA2ref, 0);
    cuStreamSynchronize(stream);

    unsigned char *ha = malloc((size_t)2560*1440*4);
    unsigned char *hb = malloc((size_t)2560*1440*4);
    dump_out(hA, 2560, 1440, ha);
    dump_out(hA2ref, 2560, 1440, hb);
    int pA2 = psnr(ha, hb, (size_t)2560*1440*4);
    printf("[RESULT A2] 仅输入变 (720p->1080p, out 1440p) PSNR = %d dB\n", pA2);

    // ── A1：输入+输出都变（1080p in, 2160p out）──────────────────────
    sr = pfn_SetImage(hA, "DstImage0", &out2160);
    printf("[A1] SetImage(DstImage0) to 2160p -> %d\n", sr);
    for (int i = 0; i < 3; i++) {
        int rr = pfn_Run(hA, 0);
        if (rr != 0) { fprintf(stderr, "A1 Run[%d] -> %d\n", i, rr); return 1; }
    }
    cuStreamSynchronize(stream);

    void *hA1ref = make_engine(1920, 1080, 3840, 2160, &in1080, &out2160, stream);
    if (!hA1ref) return 1;
    for (int i = 0; i < 3; i++) pfn_Run(hA1ref, 0);
    cuStreamSynchronize(stream);

    unsigned char *ha1 = malloc((size_t)3840*2160*4);
    unsigned char *hb1 = malloc((size_t)3840*2160*4);
    dump_out(hA, 3840, 2160, ha1);
    dump_out(hA1ref, 3840, 2160, hb1);
    int pA1 = psnr(ha1, hb1, (size_t)3840*2160*4);
    printf("[RESULT A1] 输入+输出都变 (1080p->2160p) PSNR = %d dB\n", pA1);

    // ── A3：缩小方向（1080p->720p in, 2160p->1440p out）──────────────
    void *hB = make_engine(1920, 1080, 3840, 2160, &in1080, &out2160, stream);
    if (!hB) return 1;
    printf("[A3] engine loaded 1080p->2160p\n");
    pfn_SetImage(hB, "SrcImage0", &in720);
    pfn_SetImage(hB, "DstImage0", &out1440);
    for (int i = 0; i < 3; i++) {
        int rr = pfn_Run(hB, 0);
        if (rr != 0) { fprintf(stderr, "A3 Run[%d] -> %d\n", i, rr); return 1; }
    }
    cuStreamSynchronize(stream);

    void *hBref = make_engine(1280, 720, 2560, 1440, &in720, &out1440, stream);
    if (!hBref) return 1;
    for (int i = 0; i < 3; i++) pfn_Run(hBref, 0);
    cuStreamSynchronize(stream);

    unsigned char *hb2 = malloc((size_t)2560*1440*4);
    unsigned char *hb3 = malloc((size_t)2560*1440*4);
    dump_out(hB, 2560, 1440, hb2);
    dump_out(hBref, 2560, 1440, hb3);
    int pA3 = psnr(hb2, hb3, (size_t)2560*1440*4);
    printf("[RESULT A3] 缩小 (1080p->720p, 2160p->1440p) PSNR = %d dB\n", pA3);

    printf("[VERDICT] A2=%s A1=%s A3=%s\n",
           pA2 >= 40 ? "无需重建" : "必须重建",
           pA1 >= 40 ? "无需重建" : "必须重建(vfx_test5成立)",
           pA3 >= 40 ? "无需重建" : "必须重建");

    pfn_DestroyEffect(hA); pfn_DestroyEffect(hA2ref); pfn_DestroyEffect(hA1ref);
    pfn_DestroyEffect(hB); pfn_DestroyEffect(hBref);
    pfn_Dealloc(&in720); pfn_Dealloc(&in1080);
    pfn_Dealloc(&out1440); pfn_Dealloc(&out2160);
    cuStreamDestroy(stream);
    cuCtxDestroy(ctx);
    return 0;
}
