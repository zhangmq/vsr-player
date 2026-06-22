#pragma once

namespace vsr {

/// RAII wrapper around Vulkan instance and device creation.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    /// Create Vulkan instance and select a physical device.
    bool init(void* window_surface);

    /// Get the logical device handle.
    void* device() const { return device_; }

    /// Get the physical device handle.
    void* physical_device() const { return physical_device_; }

    /// Get the graphics queue family index.
    int graphics_queue_family() const { return graphics_queue_family_; }

    /// Release Vulkan resources.
    void release();

private:
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    int graphics_queue_family_ = -1;
};

}  // namespace vsr
