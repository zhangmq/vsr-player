#include "NV12ToRGB.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda.h>
#include <nvrtc.h>

namespace vsr {

// ── CUDA kernel source ────────────────────────────────────────────────

static const char* kKernelSrc = R"(
extern "C" __global__ void nv12_to_rgb_f32(
    const unsigned char* __restrict__ y_plane,
    int y_pitch,
    const unsigned char* __restrict__ uv_plane,
    int uv_pitch,
    float* __restrict__ rgb_output,
    int width,
    int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float Y = (float)y_plane[y * y_pitch + x];

    int uv_x = x & ~1;
    int uv_y = y / 2;
    int uv_idx = uv_y * uv_pitch + uv_x;
    float U = (float)uv_plane[uv_idx];
    float V = (float)uv_plane[uv_idx + 1];

    float y_f = Y / 255.0f;
    float u_f = U / 255.0f - 0.5f;
    float v_f = V / 255.0f - 0.5f;

    // BT.601 full-range YUV -> RGB
    float r = __saturatef(y_f + 1.402f * v_f);
    float g = __saturatef(y_f - 0.34414f * u_f - 0.71414f * v_f);
    float b = __saturatef(y_f + 1.772f * u_f);

    int plane_size = width * height;
    rgb_output[0 * plane_size + y * width + x] = r;
    rgb_output[1 * plane_size + y * width + x] = g;
    rgb_output[2 * plane_size + y * width + x] = b;
}
)";

// ── Constructor / Destructor ──────────────────────────────────────────

NV12ToRGB::NV12ToRGB() = default;

NV12ToRGB::~NV12ToRGB() {
    if (module_) {
        cuModuleUnload((CUmodule)module_);
        module_ = nullptr;
        kernel_ = nullptr;
    }
}

// ── Compile CUDA kernel at runtime via NVRTC ──────────────────────────

bool NV12ToRGB::compile() {
    if (ready_) return true;

    nvrtcProgram prog;
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc, "nv12_to_rgb_f32",
                                         0, nullptr, nullptr);
    if (res != NVRTC_SUCCESS) {
        fprintf(stderr, "NV12ToRGB: nvrtcCreateProgram failed: %s\n",
                nvrtcGetErrorString(res));
        return false;
    }

    int major = 0, minor = 0;
    CUdevice dev;
    cuCtxGetDevice(&dev);
    cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);

    char arch[32];
    snprintf(arch, sizeof(arch), "--gpu-architecture=compute_%d%d", major, minor);

    const char* opts[] = {arch, "--use_fast_math"};
    res = nvrtcCompileProgram(prog, 2, opts);
    if (res != NVRTC_SUCCESS) {
        size_t log_size;
        nvrtcGetProgramLogSize(prog, &log_size);
        char* log = new char[log_size];
        nvrtcGetProgramLog(prog, log);
        fprintf(stderr, "NV12ToRGB: NVRTC compile failed:\n%s\n", log);
        delete[] log;
        nvrtcDestroyProgram(&prog);
        return false;
    }

    size_t ptx_size;
    nvrtcGetPTXSize(prog, &ptx_size);
    char* ptx = new char[ptx_size];
    nvrtcGetPTX(prog, ptx);
    nvrtcDestroyProgram(&prog);

    CUresult cu_res = cuModuleLoadData((CUmodule*)&module_, ptx);
    delete[] ptx;
    if (cu_res != CUDA_SUCCESS) {
        fprintf(stderr, "NV12ToRGB: cuModuleLoadData failed (%d)\n", cu_res);
        return false;
    }

    cu_res = cuModuleGetFunction((CUfunction*)&kernel_,
                                  (CUmodule)module_,
                                  "nv12_to_rgb_f32");
    if (cu_res != CUDA_SUCCESS) {
        fprintf(stderr, "NV12ToRGB: cuModuleGetFunction failed (%d)\n", cu_res);
        return false;
    }

    fprintf(stderr, "NV12ToRGB: kernel compiled (sm_%d%d)\n", major, minor);
    ready_ = true;
    return true;
}

// ── Convert ───────────────────────────────────────────────────────────

bool NV12ToRGB::convert(uint8_t* y_plane, int y_pitch,
                         uint8_t* uv_plane, int uv_pitch,
                         int width, int height,
                         float* rgb_output, void* stream) {
    if (!ready_ && !compile()) return false;

    int block_x = 16, block_y = 16;
    int grid_x = (width  + block_x - 1) / block_x;
    int grid_y = (height + block_y - 1) / block_y;

    void* args[] = {
        &y_plane, &y_pitch,
        &uv_plane, &uv_pitch,
        &rgb_output,
        &width, &height
    };

    CUresult res = cuLaunchKernel(
        (CUfunction)kernel_,
        grid_x, grid_y, 1,
        block_x, block_y, 1,
        0,
        (CUstream)stream,
        args, nullptr);

    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "NV12ToRGB: cuLaunchKernel failed (%d)\n", res);
        return false;
    }
    return true;
}

}  // namespace vsr
