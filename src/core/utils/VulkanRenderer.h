#pragma once

#include <cstdint>
#include <vector>

#include "VulkanContext.h"
#include "SwapchainManager.h"

namespace vsr {

/// Minimal Vulkan renderer for video frames — Wayland only.
/// Delegates instance/device/surface management to VulkanContext
/// and swapchain/render-pass/framebuffer management to SwapchainManager.
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    /// Initialize Vulkan and create Wayland surface.
    /// @param native_window  wl_surface* from Qt's winId()
    /// @param native_display wl_display* from Qt's platform native interface
    bool init(void* native_window, void* native_display);

    /// Render one frame. Video is letterboxed to maintain aspect ratio
    /// within the swapchain (which matches the window/surface size).
    /// @param is_rgba  If true, data is RGBA (4 bytes/pixel), else RGB24 (3 bytes/pixel).
    bool render_frame(const uint8_t* data, int video_w, int video_h,
                      bool is_rgba = false);

    /// Recreate swapchain at the given surface (window) size.
    bool resize(int surface_w, int surface_h);

    void release();
    bool is_ready() const { return ctx_.ready(); }

private:
    bool create_swapchain_and_pipeline(int w, int h);
    bool update_texture(const uint8_t* data, int w, int h, int bpp);

    // Owned sub-resource managers
    VulkanContext ctx_;
    SwapchainManager swapchain_;

    // Pipeline / descriptor
    void* pipeline_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* descriptor_set_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* descriptor_set_ = nullptr;

    // Texture (uploaded CPU-side data)
    void* texture_ = nullptr;
    void* texture_memory_ = nullptr;
    int tex_width_ = 0, tex_height_ = 0;
    size_t tex_row_pitch_ = 0;  // from vkGetImageSubresourceLayout

    // Cached swapchain dimensions (for letterbox calculation)
    int swapchain_width_ = 0, swapchain_height_ = 0;
};

}  // namespace vsr
