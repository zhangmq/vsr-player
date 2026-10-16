// src/mpv/video/filter/vfg_proc.h
// VFG（NVIDIA VFX SDK VideoFrameGeneration）引擎封装——与 vsr_internal.h 的
// VSR 封装同构：引擎句柄 + per-context NvCVImage 缓冲（SrcImage0/SrcImage1/
// DstImage0）、init/reconfig/process/destroy。
//
// 调用序列照 third_party/.vfx-dl/vfx13_probe.c:143-179（已实测跑通）：
//   CreateEffect("VideoFrameGeneration") → SetU32(InputWidth/InputHeight/
//   Mode/FrameMultiplier/FrameIndex/AutomaticShotChangeDetectionEnabled) →
//   SetImage(SrcImage0/SrcImage1/DstImage0) → SetCudaStream → Load → Run(0)
// 每次 Run 生成 1 张插值帧（batch size 只支持 1）。
#pragma once

#include <stdbool.h>

#include "vfx_loader.h"

// VFG 的 Mode（nvVideoEffects.h / 官方 nvvfx 包装的 Mode 枚举）
enum vfg_mode {
    VFG_MODE_LOW    = 0,
    VFG_MODE_MEDIUM = 1,
    VFG_MODE_HIGH   = 2,
};

// SDK 支持的 FrameMultiplier 范围（官方文档/包装：2..8）
#define VFG_MULT_MIN 2
#define VFG_MULT_MAX 8

struct vfg_context {
    void   *handle;        // NvVFX_Handle
    // 两张输入 + 一张输出：均为 RGBA U8 chunky GPU。SFU 角色（SrcImage0=
    // 前帧、SrcImage1=当前帧）在 Load 前 SetImage 一次后**固定不变**
    //（探针序列），内容由调用方按需 D2D 写入。
    struct NvCVImage in_img[2];
    void   *in_pixels[2];
    int     in_pitch[2];
    struct NvCVImage out_img;
    void   *out_pixels;
    int     out_pitch;
    CUstream stream;
    bool    own_stream;    // 调用方未给 stream 时自建（vf_vfg 总是借用）
    bool    ready;
    int     in_w, in_h;
    int     mode;          // enum vfg_mode
    int     multiplier;    // FrameMultiplier（每次 Run 前 SetU32）
};

// 创建 effect、绑定缓冲与流、Load。失败返回 false（并清理已建状态）。
bool vfg_init(struct vfg_context *c, int in_w, int in_h, int mode,
              int multiplier, CUstream stream);

// 输入尺寸 / Mode 变更：销毁 effect + 重建 + 重新 Load。
// 依据：官方 nvvfx 包装对 Mode / InputWidth / InputHeight 都标了
// requires_reload（Load 后改这些参数必须重新 load）；探针只在 Load **之前**
// 设过 Mode，Load 后改是否生效未验证 → 一律走重建，不赌。
bool vfg_reconfig(struct vfg_context *c, int in_w, int in_h, int mode,
                  int multiplier, CUstream stream);

// FrameMultiplier 更新（无需重建：官方包装的 run() 每次 SetU32 该参数，
// 且未标 requires_reload）。
bool vfg_set_multiplier(struct vfg_context *c, int multiplier);

// 生成当前 pair 的第 frame_index 张插值帧（1..multiplier-1，multiplier
// 模式 t = frame_index/multiplier）。成功时 *output_ptr 指向引擎输出
// 缓冲（调用方须在自己的流上完成后续拷贝/同步后再使用）。
bool vfg_process(struct vfg_context *c, int frame_index,
                 void **output_ptr, int *out_pitch);

// 销毁 effect 与缓冲；销毁前等待在途工作完成（防 GPU 写已释放缓冲）。
void vfg_destroy(struct vfg_context *c);
