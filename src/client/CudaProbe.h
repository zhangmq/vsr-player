#pragma once
#include <cstdint>
#include <string>

/// CUDA 设备探测（**客户端不链接 libcuda**：运行时 dlopen "+libcuda.so.1"+，
/// 没有 NVIDIA 驱动的机器上安静返回 +driverOk=false+）。
///
/// 为什么要这个：我们的超分/插帧（VFX VSR/VFG）与 nvdec 都在 CUDA 上，
/// 输出帧以外部显存句柄零拷贝交给 Vulkan VO 导入 —— 这要求 **CUDA 设备与
/// Vulkan 物理设备是同一张卡**（mpv 自己的 cuda_vk 也是按 UUID 匹配的：
/// video/out/hwdec/hwdec_cuda_vk.c）。多卡机器（笔记本 iGPU+dGPU、
/// CUDA_VISIBLE_DEVICES 只暴露其中一张）上两者可能不是同一张，此时继续跑
/// 滤镜只会得到黑屏 —— 所以启动时用 UUID 判一次，判不出来就把 AI 降级为直通。
struct CudaProbeResult {
    bool driverOk = false;      // libcuda 可加载、cuInit 成功、至少 1 张卡可见
    bool matched = false;       // 存在 UUID 与 Vulkan 设备相同的 CUDA 设备
    int deviceCount = 0;        // CUDA 可见设备数（受 CUDA_VISIBLE_DEVICES 影响）
    std::string matchedName;    // matched 时的设备名
    std::string dev0Name;       // CUDA 0 号设备名（hwup 无 UUID 时的默认选择）
    bool dev0Matches = false;   // CUDA 0 号的 UUID 是否等于 Vulkan 设备 UUID
    std::string detail;         // 诊断串（失败原因 / 可见设备列表）
};

/// uuid = Vulkan 物理设备的 16 字节 deviceUUID（VkPhysicalDeviceIDProperties）。
CudaProbeResult cuda_probe_for_vulkan_uuid(const uint8_t uuid[16]);
