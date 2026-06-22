#include "VSRProcessor.h"

#include <cstdio>
#include <cstring>
#include <cuda.h>
#include <dlfcn.h>
#include <vector>

#include <cuda.h>

#include "nvVideoEffects.h"

namespace vsr {

static unsigned int quality_to_vfx(Quality q, bool denoise) {
    if (denoise) {
        switch (q) {
            case Quality::LOW:    return 8;
            case Quality::MEDIUM: return 9;
            case Quality::HIGH:   return 10;
            case Quality::ULTRA:  return 11;
        }
    }
    switch (q) {
        case Quality::LOW:    return 1;
        case Quality::MEDIUM: return 2;
        case Quality::HIGH:   return 3;
        case Quality::ULTRA:  return 4;
    }
    return 3;
}

// ── Function pointer table ────────────────────────────────────────────

static void* g_vfx_lib = nullptr;
static void* g_nvcv_lib = nullptr;

#define DECL_FN(F) static decltype(&F) pfn_##F;
DECL_FN(NvVFX_CreateEffect)
DECL_FN(NvVFX_DestroyEffect)
DECL_FN(NvVFX_SetU32)
DECL_FN(NvVFX_SetImage)
DECL_FN(NvVFX_GetImage)
DECL_FN(NvVFX_SetCudaStream)
DECL_FN(NvVFX_Load)
DECL_FN(NvVFX_Run)
DECL_FN(NvCVImage_Alloc)
DECL_FN(NvCVImage_Dealloc)
DECL_FN(NvCVImage_Transfer)
#undef DECL_FN

#define LOAD_SYM(LIB, NAME)                                         \
    do {                                                             \
        pfn_##NAME = reinterpret_cast<decltype(pfn_##NAME)>(        \
            dlsym(LIB, #NAME));                                      \
        if (!pfn_##NAME) {                                           \
            fprintf(stderr, "VSR: dlsym("#NAME") failed: %s\n",    \
                    dlerror());                                      \
            return false;                                            \
        }                                                            \
    } while (0)

static bool load_nvvfx_libraries() {
    if (g_vfx_lib) return true;

    const char* search[] = {
        "third_party/nvvfx/lib/",
        "build/lib/",
        "/home/zmq/miniforge3/envs/vsr-player/lib/python3.12/site-packages/nvvfx/libs/",
        "",
    };

    for (int i = 0; i < 4; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", search[i], "libVideoFX.so");
        g_vfx_lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (g_vfx_lib) break;
    }
    if (!g_vfx_lib) {
        fprintf(stderr, "VSR: libVideoFX.so not found: %s\n", dlerror());
        return false;
    }

    for (int i = 0; i < 4; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%s", search[i], "libNVCVImage.so");
        g_nvcv_lib = dlopen(path, RTLD_NOW);
        if (g_nvcv_lib) break;
    }
    if (!g_nvcv_lib) {
        fprintf(stderr, "VSR: libNVCVImage.so not found: %s\n", dlerror());
        dlclose(g_vfx_lib); g_vfx_lib = nullptr;
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

// ── Constructor / Destructor ──────────────────────────────────────────

VSRProcessor::VSRProcessor() = default;
VSRProcessor::~VSRProcessor() { release(); }

// ── Init ──────────────────────────────────────────────────────────────

bool VSRProcessor::init(int in_w, int in_h, int out_w, int out_h, Quality quality) {
    if (!load_nvvfx_libraries()) return false;

    in_w_ = in_w; in_h_ = in_h;
    out_w_ = out_w; out_h_ = out_h;
    quality_ = quality;

    // Create effect — the .so shipped with nvidia-vfx Python package
    // responds to "VideoSuperRes" (our MIT-licensed header says "SuperRes"
    // but that's from an older SDK version).
    int ret = pfn_NvVFX_CreateEffect("VideoSuperRes", &vsr_handle_);
    if (ret != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: CreateEffect(VideoSuperRes) failed (%d: %s)\n",
                ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
        return false;
    }

    // Mode: same-resolution = denoise, upscaled = super-resolution
    bool denoise = (in_w_ == out_w_ && in_h_ == out_h_);
    unsigned int qv = quality_to_vfx(quality_, denoise);
    // Python nvvfx uses "QualityLevel" (not "Strength") to select the
    // AI model.  Default is BICUBIC=0 — no AI enhancement at all.
    // Reference: nvvfx/effects/video_super_res.py `_PARAM_QUALITY`
    pfn_NvVFX_SetU32(vsr_handle_, "QualityLevel", qv);
    fprintf(stderr, "VSR: [%s] %dx%d→%dx%d quality=%u %s\n",
            denoise ? "DENOISE" : "UPSCALE",
            in_w_, in_h_, out_w_, out_h_, qv,
            denoise ? "(1:1 denoising)" : "(AI super-resolution)");

    // ── Output image: per sample code, VSR accepts RGBA U8 interleaved ──
    // Reference: NVIDIA-Maxine/VFX-SDK-Samples/apps/VideoEffectsApp/
    //   "NGX VSR supports RGBA/BGRA U8 interleaved format on GPU"
    out_img_ = NvCVImage{};
    int ar = pfn_NvCVImage_Alloc(&out_img_, out_w_, out_h_,
                                   NVCV_RGBA, NVCV_U8,
                                   NVCV_CHUNKY, NVCV_GPU, 32);
    if (ar != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: Alloc(output RGBA U8) failed (%d)\n", ar);
        return false;
    }
    ret = pfn_NvVFX_SetImage(vsr_handle_, NVVFX_OUTPUT_IMAGE, &out_img_);
    fprintf(stderr, "VSR: DstImage0 RGBA U8 → %d (%s)\n",
            ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
    if (ret != NVCV_SUCCESS) {
        pfn_NvCVImage_Dealloc(&out_img_);
        out_img_ = NvCVImage{};
        return false;
    }

    // ── Input image: RGBA U8 chunky GPU (per sample code) ──
    // Allocate once and reuse across all frames.
    {
        int ar = pfn_NvCVImage_Alloc(&in_img_, in_w_, in_h_,
                                       NVCV_RGBA, NVCV_U8,
                                       NVCV_CHUNKY, NVCV_GPU, 32);
        if (ar != NVCV_SUCCESS) {
            fprintf(stderr, "VSR: Alloc(input RGBA U8) failed (%d)\n", ar);
            return false;
        }
        ret = pfn_NvVFX_SetImage(vsr_handle_, NVVFX_INPUT_IMAGE, &in_img_);
        if (ret != NVCV_SUCCESS) {
            fprintf(stderr, "VSR: SetImage(input RGBA U8) failed (%d: %s)\n",
                    ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
            pfn_NvCVImage_Dealloc(&in_img_);
            in_img_ = NvCVImage{};
            return false;
        }
        input_allocated_ = true;
    }

    // ── Temp buffer for NvCVImage_Transfer ──
    // Pre-allocate an empty GPU buffer. Transfer will auto-size it as needed.
    // Following sample code pattern: one temp buffer reused across frames.
    tmp_img_ = NvCVImage{};
    int max_dim = (out_w_ > in_w_) ? out_w_ : in_w_;
    int max_h = (out_h_ > in_h_) ? out_h_ : in_h_;
    ar = pfn_NvCVImage_Alloc(&tmp_img_, max_dim, max_h,
                               NVCV_RGBA, NVCV_U8,
                               NVCV_CHUNKY, NVCV_GPU, 32);
    if (ar != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: Alloc(temp) failed (%d) — will use ephemeral\n", ar);
        tmp_img_ = NvCVImage{};  // fallback: empty, auto-alloc
    }

    // ── CUDA stream ──
    // Create a per-effect CUDA stream if caller hasn't provided one via set_stream()
    if (!cuda_stream_) {
        cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);
        own_stream_ = true;
    }
    pfn_NvVFX_SetCudaStream(vsr_handle_, NVVFX_CUDA_STREAM, (CUstream)cuda_stream_);

    // ── Load model ──
    ret = pfn_NvVFX_Load(vsr_handle_);
    if (ret != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: Load failed (%d: %s)\n",
                ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
        return false;
    }

    // ── Warmup frames (Python nvidia-vfx does this after load()) ──
    // Python uses torch.rand() — random data is critical: constant input
    // (grey) doesn't exercise the model's edge/texture kernels, leaving
    // temporal state in a degenerate condition that causes blurry output.
    // Reference: archive/python-v1/vsr_player/vsr_pipeline.py `_warmup()`
    {
        size_t in_bytes = (size_t)in_img_.pitch * in_h_;
        std::vector<uint8_t> random_rgba(in_bytes);
        // Simple LCG for deterministic-but-noisy pattern
        unsigned int seed = 42;
        for (size_t i = 0; i < in_bytes; i += 4) {
            seed = seed * 1103515245 + 12345;
            random_rgba[i + 0] = (uint8_t)((seed >> 16) & 0xFF);  // R
            seed = seed * 1103515245 + 12345;
            random_rgba[i + 1] = (uint8_t)((seed >> 16) & 0xFF);  // G
            seed = seed * 1103515245 + 12345;
            random_rgba[i + 2] = (uint8_t)((seed >> 16) & 0xFF);  // B
            random_rgba[i + 3] = 255;                               // A = opaque
        }
        cuMemcpyHtoD((CUdeviceptr)in_img_.pixels, random_rgba.data(), in_bytes);

        for (int wu = 0; wu < 3; wu++) {
            int wr = pfn_NvVFX_Run(vsr_handle_, 0);
            if (wr != NVCV_SUCCESS) {
                fprintf(stderr, "VSR: warmup[%d] Run → %d (%s)\n",
                        wu, wr, NvCV_GetErrorStringFromCode((NvCV_Status)wr));
            }
        }
        fprintf(stderr, "VSR: warmup complete (3 frames with random data)\n");
    }

    fprintf(stderr, "VSR: init ok\n");
    return true;
}

// ── Process ───────────────────────────────────────────────────────────

bool VSRProcessor::process(void* input_cuda_ptr, void** output_cuda_ptr,
                           int* out_w, int* out_h, int* out_pitch) {
    if (!vsr_handle_) return false;

    int ret;

    // ── Build planar F32 RGB source descriptor from NV12ToRGB output ──
    // NV12ToRGB output layout: 3 contiguous planes of H×W floats
    // [R: H×W][G: H×W][B: H×W], all in one GPU allocation
    NvCVImage src{};
    src.width          = (unsigned)in_w_;
    src.height         = (unsigned)in_h_;
    src.pitch          = in_w_ * (int)sizeof(float);  // row stride within one plane
    src.pixelFormat    = NVCV_RGB;
    src.componentType  = NVCV_F32;
    src.pixelBytes     = (unsigned char)sizeof(float);   // bytes per pixel in a plane
    src.componentBytes = (unsigned char)sizeof(float);   // bytes per component
    src.numComponents  = 3;                              // R, G, B
    src.planar         = NVCV_PLANAR;                    // 3 separate planes
    src.gpuMem         = NVCV_GPU;
    src.colorspace     = 0;
    src.pixels         = input_cuda_ptr;   // → R plane
    src.deletePtr      = nullptr;
    src.deleteProc     = nullptr;
    src.bufferBytes    = (unsigned long long)in_w_ * in_h_ * 3 * sizeof(float);

    // ── Initialize alpha channel to 255 (opaque) before Transfer ──
    // NvCVImage_Transfer RGB→RGBA does not modify alpha (per header docs),
    // so we must pre-fill it. Set ALL bytes to 255: this gives R=G=B=255,A=255
    // for any pixel that Transfer doesn't overwrite, and ensures A=255 for all.
    {
        size_t rgba_bytes = (size_t)in_img_.pitch * in_h_;
        cuMemsetD8((CUdeviceptr)in_img_.pixels, 255, rgba_bytes);
    }

    // ── Transfer: planar F32 RGB → chunky U8 RGBA ──
    // scale=255.0 converts float [0,1] to uint8 [0,255]
    int tr = pfn_NvCVImage_Transfer(&src, &in_img_, 255.0f,
                                     (CUstream)cuda_stream_, &tmp_img_);
    if (tr != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: Transfer(planar f32→chunky u8 RGBA) failed (%d: %s)\n",
                tr, NvCV_GetErrorStringFromCode((NvCV_Status)tr));
        return false;
    }

    // Ensure Transfer is complete before Run reads the input buffer.
    // Transfer runs on cuda_stream_ (non-blocking); Run(sync) may not
    // implicitly synchronize with non-default streams on all drivers.
    if (cuda_stream_) {
        cuStreamSynchronize((CUstream)cuda_stream_);
    }

    // ── Run VSR ──
    ret = pfn_NvVFX_Run(vsr_handle_, 0);  // 0 = synchronous
    if (ret != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: Run failed (%d: %s)\n",
                ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
        return false;
    }

    // ── Get output image descriptor ──
    ret = pfn_NvVFX_GetImage(vsr_handle_, NVVFX_OUTPUT_IMAGE, &out_img_);
    if (ret != NVCV_SUCCESS) {
        fprintf(stderr, "VSR: GetImage(output) failed (%d: %s)\n",
                ret, NvCV_GetErrorStringFromCode((NvCV_Status)ret));
        return false;
    }

    *output_cuda_ptr = out_img_.pixels;
    *out_w = (int)out_img_.width;
    *out_h = (int)out_img_.height;
    if (out_pitch) *out_pitch = (int)out_img_.pitch;

    return true;
}

// ── Reconfigure ───────────────────────────────────────────────────────

bool VSRProcessor::reconfigure(int out_w, int out_h, Quality quality) {
    fprintf(stderr, "VSR: reconfigure — in=%dx%d out=%dx%d quality=%d\n",
            in_w_, in_h_, out_w, out_h, (int)quality);
    // Save dimensions before release() zeros them
    int saved_in_w = in_w_, saved_in_h = in_h_;
    release();
    bool ok = init(saved_in_w, saved_in_h, out_w, out_h, quality);
    fprintf(stderr, "VSR: reconfigure → %s\n", ok ? "ok" : "FAILED");
    return ok;
}

// ── Release ───────────────────────────────────────────────────────────

void VSRProcessor::release() {
    if (vsr_handle_) {
        pfn_NvVFX_DestroyEffect(vsr_handle_);
        vsr_handle_ = nullptr;
    }
    if (out_img_.pixels) { pfn_NvCVImage_Dealloc(&out_img_); out_img_ = NvCVImage{}; }
    if (in_img_.pixels)  { pfn_NvCVImage_Dealloc(&in_img_);  in_img_  = NvCVImage{}; }
    if (tmp_img_.pixels) { pfn_NvCVImage_Dealloc(&tmp_img_); tmp_img_ = NvCVImage{}; }
    if (own_stream_ && cuda_stream_) {
        cuStreamDestroy((CUstream)cuda_stream_);
        cuda_stream_ = nullptr;
        own_stream_ = false;
    }
    input_allocated_ = false;
    in_w_ = in_h_ = out_w_ = out_h_ = 0;
}

}  // namespace vsr
