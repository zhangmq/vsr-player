// src/mpv/video/filter/vfx_loader.h
// VFX（NVIDIA Video Effects SDK）库定位与加载层——VSR（vsr_proc.c）、
// VFG（vfg_proc.c）与将来的 TrueHDR 三家共用的**单一事实源**：目录搜索
// 顺序、dlopen 顺序与标志、dlsym 符号集合、失败语义都只在 vfx_loader.c
// 实现一次。本头暴露 SDK 类型（NvCVImage / PFN_*）、库句柄与函数指针
// （extern），以及加载入口。
#pragma once

#include <cuda.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── NvCVImage（nvCVImage.h 的结构布局，C 兼容重定义）──────────────────
// 图像缓冲为 per-context（多实例共存——退役队列延迟销毁方案）：每个
// vsr_context 持有自己的 in/out/tmp 缓冲，SDK 引擎的 SetImage 引用
// 各实例独立的 pixels 地址，销毁旧实例不影响新实例。

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

// ── nvCVImage.h 的枚举常量（C 兼容重定义）─────────────────────────────
// VSR（vsr_proc.c）与 VFG（vfg_proc.c）共用——原在 vsr_proc.c 定义，
// 搬到此层与 NvCVImage 同处，避免第二份定义分叉（数值照 nvCVImage.h）。
#define NVCV_RGB    4
#define NVCV_RGBA   6
#define NVCV_U8     1
#define NVCV_F32    7
#define NVCV_PLANAR 1
#define NVCV_CHUNKY 0
#define NVCV_GPU    1

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

// ── 库句柄与符号表（进程内单例；名字与搬移前保持一致）────────────────

extern void *g_vfx_lib;
extern void *g_nvcv_lib;

extern PFN_NvVFX_CreateEffect    pfn_NvVFX_CreateEffect;
extern PFN_NvVFX_DestroyEffect   pfn_NvVFX_DestroyEffect;
extern PFN_NvVFX_SetU32          pfn_NvVFX_SetU32;
extern PFN_NvVFX_SetImage        pfn_NvVFX_SetImage;
extern PFN_NvVFX_GetImage        pfn_NvVFX_GetImage;
extern PFN_NvVFX_SetCudaStream   pfn_NvVFX_SetCudaStream;
extern PFN_NvVFX_Load            pfn_NvVFX_Load;
extern PFN_NvVFX_Run             pfn_NvVFX_Run;
extern PFN_NvCVImage_Alloc       pfn_NvCVImage_Alloc;
extern PFN_NvCVImage_Dealloc     pfn_NvCVImage_Dealloc;
extern PFN_NvCVImage_Transfer    pfn_NvCVImage_Transfer;

// 定位并加载 VFX 库 + 解析上述符号；已加载时直接返回 true（幂等）。
bool vfx_load_libraries(void);

#ifdef __cplusplus
}
#endif
