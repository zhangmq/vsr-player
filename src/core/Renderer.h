#pragma once

namespace vsr {

/// Vulkan render pipeline. Renders VSR output frames to the display surface.
class Renderer {
public:
    Renderer();
    ~Renderer();

    /// Initialize Vulkan device, swapchain, render passes.
    bool init(void* vulkan_surface, int width, int height);

    /// Render one RGBA frame (CUDA device ptr) to the swapchain.
    bool render_frame(void* rgba_cuda_ptr, int width, int height);

    /// Handle window resize.
    bool resize(int width, int height);

    /// Release Vulkan resources.
    void release();

private:
    void* instance_ = nullptr;
    void* device_ = nullptr;
    void* surface_ = nullptr;
    void* swapchain_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace vsr
