#pragma once

#include <cstdint>
#include <vector>

namespace vsr {

/// Minimal Vulkan renderer for video frames.
///
/// Creates a fullscreen textured quad pipeline. Call render_frame() with
/// RGB24 data to upload as a texture and present to the swapchain.
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    /// Initialize Vulkan and create swapchain for the given window.
    /// @param window  platform-specific window handle (X11 Window or Wayland surface)
    /// @param display platform display connection (X11 Display*, nullptr for Wayland)
    bool init(void* window, void* display = nullptr);

    /// Render one RGB24 frame. Data layout: R,G,B interleaved, top-down.
    bool render_frame(const uint8_t* rgb_data, int width, int height);

    /// Handle window resize. Recreates swapchain.
    bool resize(int width, int height);

    /// Release all Vulkan resources.
    void release();

    bool is_ready() const { return device_ != nullptr; }

private:
    // Internal: create swapchain + pipeline (also called on resize)
    bool create_swapchain_and_pipeline(int w, int h);

    // Internal: upload RGB data as Vulkan texture
    bool update_texture(const uint8_t* data, int w, int h);
    // Handle types (opaque, cast from Vulkan handles)
    void* instance_ = nullptr;       // VkInstance
    void* physical_device_ = nullptr;
    void* device_ = nullptr;         // VkDevice
    void* surface_ = nullptr;        // VkSurfaceKHR
    void* swapchain_ = nullptr;
    void* render_pass_ = nullptr;
    void* pipeline_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* descriptor_set_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* descriptor_set_ = nullptr;
    void* command_pool_ = nullptr;
    void* command_buffer_ = nullptr;
    void* sampler_ = nullptr;

    // Swapchain
    std::vector<void*> swapchain_images_;
    std::vector<void*> swapchain_image_views_;
    std::vector<void*> framebuffers_;
    int swapchain_width_ = 0;
    int swapchain_height_ = 0;

    // Video texture
    void* texture_ = nullptr;
    void* texture_memory_ = nullptr;
    int tex_width_ = 0;
    int tex_height_ = 0;

    // Queue
    void* queue_ = nullptr;
    int queue_family_ = -1;

    // Sync
    void* image_available_semaphore_ = nullptr;
    void* render_finished_semaphore_ = nullptr;
    void* fence_ = nullptr;
};

}  // namespace vsr
