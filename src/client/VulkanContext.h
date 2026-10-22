#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/log.h>

/// Owns the VkInstance + shared VkDevice (via libplacebo pl_vulkan_create).
/// RAII: destroys plVk/log/instance in dtor.
class VulkanContext {
public:
    ~VulkanContext();

    /// Create instance → pick first physical device → enable features
    /// (timeline semaphore) → pl_vulkan_create (single graphics queue).
    /// Returns false with message printed to stderr on failure.
    bool init();

    VkInstance instance() const { return inst_; }
    VkPhysicalDevice physicalDevice() const { return pd_; }
    VkDevice device() const { return dev_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamilyIndex() const { return qfi_; }
    const VkPhysicalDeviceFeatures2 *features() const { return &f2_; }
    /// Device-level extensions enabled on dev_ (for mpv's pl_vulkan_import —
    /// 必须如实上报，否则 libplacebo 不加载 FD 导出/导入函数 → SIGSEGV).
    const char *const *deviceExtensions(int *num) const {
        *num = (int)devExts_.size();
        return devExts_.data();
    }
    /// Physical device name (for the OSD GPU line).
    const char *deviceName() const { return devName_.c_str(); }
    /// 物理设备 UUID（VkPhysicalDeviceIDProperties.deviceUUID）。用于和 CUDA
    /// 设备做同一性判断：我们的超分/插帧是 CUDA 产出 + Vulkan 零拷贝导入，
    /// 两者必须是同一张卡（详见 main.cpp 的 AI 可用性检查）。
    const uint8_t *deviceUuid() const { return uuid_; }

    /// VK_EXT_swapchain_colorspace 是否已在该 instance 上启用。false = 该环境
    /// 不可能枚举 HDR 色彩空间（Qt 的 isFormatSupported 恒 false）→ HDR 上屏
    /// 直接退 SDR，不必再往下探测。
    bool swapchainColorspaceEnabled() const { return swapchainColorspaceExt_; }

private:
    mutable std::string devName_;   // filled in init()
    uint8_t uuid_[16] = {};         // filled in init()（VkPhysicalDeviceIDProperties）

private:
    bool swapchainColorspaceExt_ = false;
    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qfi_ = 0;
    pl_vulkan plVk_ = nullptr;
    pl_log plLog_ = nullptr;

    VkPhysicalDeviceVulkan12Features vk12_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features vk13_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &vk12_ };
    VkPhysicalDeviceFeatures2 f2_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vk13_ };

    std::vector<const char *> devExts_;
};
