#pragma once

#include <cstdint>

namespace vsr {

/// RAII wrapper around Vulkan instance, device, and basic resources.
/// Owns instance, physical device, device, Wayland surface, queue,
/// command pool, command buffer, sync primitives, and sampler.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    /// Create Vulkan instance, Wayland surface, select physical device,
    /// create logical device, command pool, sync primitives, sampler.
    /// @param native_window  wl_surface* from Qt's winId()
    /// @param native_display wl_display* from Qt's platform native interface
    bool init(void* native_window, void* native_display);
    void release();
    bool ready() const { return device_ != nullptr; }

    // Accessors — return opaque void*; callers cast to VkXxx types.
    void* instance() const { return instance_; }
    void* physicalDevice() const { return physical_device_; }
    void* device() const { return device_; }
    void* surface() const { return surface_; }
    void* queue() const { return queue_; }
    int queueFamily() const { return queue_family_; }
    void* commandPool() const { return command_pool_; }
    void* commandBuffer() const { return command_buffer_; }
    void* sampler() const { return sampler_; }
    void* imageAvailableSemaphore() const { return image_available_semaphore_; }
    void* renderFinishedSemaphore() const { return render_finished_semaphore_; }
    void* fence() const { return fence_; }

private:
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    void* surface_ = nullptr;
    void* queue_ = nullptr;
    int queue_family_ = -1;
    void* command_pool_ = nullptr;
    void* command_buffer_ = nullptr;
    void* sampler_ = nullptr;
    void* image_available_semaphore_ = nullptr;
    void* render_finished_semaphore_ = nullptr;
    void* fence_ = nullptr;
};

/// Helper: find a memory type matching given type_bits and properties.
/// Used by VulkanRenderer for texture memory allocation.
uint32_t find_memory_type(void* physical_device, uint32_t type_bits,
                          uint32_t memory_property_flags);

}  // namespace vsr
