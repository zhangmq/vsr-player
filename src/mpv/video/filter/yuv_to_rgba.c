// yuv_to_rgba.c — CUDA YUV(NV12/P010/P016)→chunky U8 RGBA via NVRTC
// Parameterized color matrix + range, compiled into kernel as #defines.

#include "yuv_to_rgba.h"

#include <cuda.h>
#include <nvrtc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kKernelSrc =
"extern \"C\" __global__ void yuv_to_rgba_u8(\n"
"    const unsigned char* __restrict__ y_plane,\n"
"    int y_pitch,\n"
"    const unsigned char* __restrict__ uv_plane,\n"
"    int uv_pitch,\n"
"    unsigned char* __restrict__ rgba_output,\n"
"    int rgba_pitch,\n"
"    int width,\n"
"    int height)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= width || y >= height) return;\n"
"\n"
"    float Y, U, V;\n"
"    if (BIT_DEPTH <= 8) {\n"
"        Y = (float)y_plane[y * y_pitch + x];\n"
"    } else {\n"
"        const unsigned short* y16 = (const unsigned short*)y_plane;\n"
"        Y = (float)y16[y * y_pitch / 2 + x];\n"
"    }\n"
"\n"
"    int uv_x = x & ~1;\n"
"    int uv_y = y / 2;\n"
"    if (BIT_DEPTH <= 8) {\n"
"        U = (float)uv_plane[uv_y * uv_pitch + uv_x];\n"
"        V = (float)uv_plane[uv_y * uv_pitch + uv_x + 1];\n"
"    } else {\n"
"        const unsigned short* uv16 = (const unsigned short*)uv_plane;\n"
"        int idx = uv_y * (uv_pitch / 2) + uv_x;\n"
"        U = (float)uv16[idx];\n"
"        V = (float)uv16[idx + 1];\n"
"    }\n"
"\n"
"    // BIT_DEPTH = 样本存储位（mpv imgfmt_desc.bpp[0]）。\n"
"    // P010/P016（16）：10bit 数据 MSB 对齐（低 6 位清 0，见 mpv\n"
"    // img_format.c：P010 has the lower 6 bits cleared to 0）——\n"
"    // 值域必须按 16bit 满幅（65535），10bit 的 limited 值\n"
"    // （64/512/1023）左对齐后为 4096/32768/65472。按 10bit 右对齐\n"
"    // 归一化会把左对齐值放大 64 倍 → 饱和 → 品红。\n"
"    float y_max = (BIT_DEPTH <= 8) ? 255.0f\n"
"                 : (BIT_DEPTH >= 16) ? 65535.0f : 1023.0f;\n"
"    float uv_max = y_max;\n"
"\n"
"    if (YUV_RANGE_LIMITED) {\n"
"        float y_min  = (BIT_DEPTH <= 8) ? 16.0f\n"
"                      : (BIT_DEPTH >= 16) ? 4096.0f : 64.0f;\n"
"        float uv_min = y_min;\n"
"        float uv_mid = (BIT_DEPTH <= 8) ? 128.0f\n"
"                      : (BIT_DEPTH >= 16) ? 32768.0f : 512.0f;\n"
"        Y = (Y - y_min) / (y_max - y_min);\n"
"        U = (U - uv_mid) / (uv_max - uv_min);\n"
"        V = (V - uv_mid) / (uv_max - uv_min);\n"
"    } else {\n"
"        Y = Y / y_max;\n"
"        U = U / uv_max - 0.5f;\n"
"        V = V / uv_max - 0.5f;\n"
"    }\n"
"\n"
"    float r = __saturatef(Y + K_RV * V);\n"
"    float g = __saturatef(Y - K_GU * U - K_GV * V);\n"
"    float b = __saturatef(Y + K_BU * U);\n"
"\n"
"    int out_idx = y * rgba_pitch + x * 4;\n"
"    rgba_output[out_idx + 0] = (unsigned char)(r * 255.0f + 0.5f);\n"
"    rgba_output[out_idx + 1] = (unsigned char)(g * 255.0f + 0.5f);\n"
"    rgba_output[out_idx + 2] = (unsigned char)(b * 255.0f + 0.5f);\n"
"    rgba_output[out_idx + 3] = 255;\n"
"}\n";

// ── Matrix coefficient helpers ─────────────────────────────────────────

static void get_matrix_defines(enum yuv_matrix m,
                                const char **k_rv, const char **k_gu,
                                const char **k_gv, const char **k_bu)
{
    // Kr + Kg + Kb = 1.0 for all matrices, coefficients pre-computed:
    // K_RV = 2*(1-Kr), K_GU = 2*(1-Kb)*Kb/Kg, K_GV = 2*(1-Kr)*Kr/Kg, K_BU = 2*(1-Kb)
    static const char *rv_601 = "1.402f",  *gu_601 = "0.34414f";
    static const char *gv_601 = "0.71414f", *bu_601 = "1.772f";
    static const char *rv_709 = "1.5748f",  *gu_709 = "0.18732f";
    static const char *gv_709 = "0.46812f", *bu_709 = "1.8556f";
    static const char *rv_2020 = "1.4746f",  *gu_2020 = "0.16455f";
    static const char *gv_2020 = "0.57135f", *bu_2020 = "1.8814f";

    switch (m) {
    case YUV_MATRIX_BT709:
        *k_rv = rv_709; *k_gu = gu_709; *k_gv = gv_709; *k_bu = bu_709; break;
    case YUV_MATRIX_BT2020:
        *k_rv = rv_2020; *k_gu = gu_2020; *k_gv = gv_2020; *k_bu = bu_2020; break;
    default:
        *k_rv = rv_601; *k_gu = gu_601; *k_gv = gv_601; *k_bu = bu_601; break;
    }
}

// ── Init ───────────────────────────────────────────────────────────────

bool yuv_to_rgba_init(struct yuv_to_rgba *c, int bit_depth,
                       enum yuv_matrix matrix, enum yuv_range range)
{
    if (c->ready) return true;

    nvrtcProgram prog;
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc,
                                         "yuv_to_rgba_u8", 0, NULL, NULL);
    if (res != NVRTC_SUCCESS) {
        fprintf(stderr, "yuv_to_rgba: nvrtcCreateProgram failed: %s\n",
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

    const char *k_rv, *k_gu, *k_gv, *k_bu;
    get_matrix_defines(matrix, &k_rv, &k_gu, &k_gv, &k_bu);

    char bd_str[32], range_str[32];
    snprintf(bd_str, sizeof(bd_str), "-DBIT_DEPTH=%d", bit_depth);
    snprintf(range_str, sizeof(range_str), "-DYUV_RANGE_LIMITED=%d",
             range == YUV_RANGE_LIMITED ? 1 : 0);

    char krv_str[64], kgu_str[64], kgv_str[64], kbu_str[64];
    snprintf(krv_str, sizeof(krv_str), "-DK_RV=%s", k_rv);
    snprintf(kgu_str, sizeof(kgu_str), "-DK_GU=%s", k_gu);
    snprintf(kgv_str, sizeof(kgv_str), "-DK_GV=%s", k_gv);
    snprintf(kbu_str, sizeof(kbu_str), "-DK_BU=%s", k_bu);

    const char *opts[] = {arch, "--use_fast_math",
                          bd_str, range_str,
                          krv_str, kgu_str, kgv_str, kbu_str};
    int nopts = 8;

    res = nvrtcCompileProgram(prog, nopts, opts);
    if (res != NVRTC_SUCCESS) {
        size_t log_size;
        nvrtcGetProgramLogSize(prog, &log_size);
        char *log = (char *)malloc(log_size);
        nvrtcGetProgramLog(prog, log);
        fprintf(stderr, "yuv_to_rgba: NVRTC compile failed:\n%s\n", log);
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
        fprintf(stderr, "yuv_to_rgba: cuModuleLoadData failed (%d)\n", cu_res);
        return false;
    }

    cu_res = cuModuleGetFunction(&c->kernel, c->module, "yuv_to_rgba_u8");
    if (cu_res != CUDA_SUCCESS) {
        fprintf(stderr, "yuv_to_rgba: cuModuleGetFunction failed (%d)\n", cu_res);
        return false;
    }

    fprintf(stderr, "yuv_to_rgba: kernel compiled (sm_%d%d, bd=%d)\n",
            major, minor, bit_depth);
    c->ready = true;
    return true;
}

// ── Convert ────────────────────────────────────────────────────────────

bool yuv_to_rgba_convert(struct yuv_to_rgba *c,
                          void *y_plane,   int y_pitch,
                          void *uv_plane,  int uv_pitch,
                          int width, int height,
                          void *rgba_output, int rgba_pitch,
                          CUstream stream)
{
    if (!c->ready) return false;

    int block_x = 16, block_y = 16;
    int grid_x = (width  + block_x - 1) / block_x;
    int grid_y = (height + block_y - 1) / block_y;

    void *args[] = {
        &y_plane, &y_pitch,
        &uv_plane, &uv_pitch,
        &rgba_output, &rgba_pitch,
        &width, &height
    };

    CUresult res = cuLaunchKernel(c->kernel,
        grid_x, grid_y, 1,
        block_x, block_y, 1,
        0, stream, args, NULL);

    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "yuv_to_rgba: cuLaunchKernel failed (%d)\n", res);
        return false;
    }
    return true;
}

// ── Destroy ────────────────────────────────────────────────────────────

void yuv_to_rgba_destroy(struct yuv_to_rgba *c)
{
    if (c->module) {
        cuModuleUnload(c->module);
        c->module = NULL;
        c->kernel = NULL;
    }
    c->ready = false;
}
