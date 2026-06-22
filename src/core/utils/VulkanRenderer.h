#pragma once

#include <cstdint>
#include <vector>

namespace vsr {

/// Minimal Vulkan renderer for video frames — Wayland only.
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    /// Initialize Vulkan and create Wayland surface.
    /// @param native_window  wl_surface* from Qt's winId()
    /// @param native_display wl_display* from Qt's platform native interface
    bool init(void* native_window, void* native_display);

    /// Render one RGB24 frame. Video is letterboxed to maintain aspect ratio
    /// within the swapchain (which matches the window/surface size).
    bool render_frame(const uint8_t* rgb_data, int video_w, int video_h);

    /// Recreate swapchain at the given surface (window) size.
    bool resize(int surface_w, int surface_h);

    void release();
    bool is_ready() const { return device_ != nullptr; }

private:
    bool create_swapchain_and_pipeline(int w, int h);
    bool update_texture(const uint8_t* data, int w, int h);

    // Handles
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    void* surface_ = nullptr;      // VkSurfaceKHR
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

    std::vector<void*> swapchain_images_;
    std::vector<void*> swapchain_image_views_;
    std::vector<void*> framebuffers_;
    int swapchain_width_ = 0, swapchain_height_ = 0;

    void* texture_ = nullptr;
    void* texture_memory_ = nullptr;
    int tex_width_ = 0, tex_height_ = 0;

    void* queue_ = nullptr;
    int queue_family_ = -1;

    void* image_available_semaphore_ = nullptr;
    void* render_finished_semaphore_ = nullptr;
    void* fence_ = nullptr;
};

}  // namespace vsr
