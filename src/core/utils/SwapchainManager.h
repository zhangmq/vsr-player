#pragma once

#include <cstdint>
#include <vector>

namespace vsr {

/// Manages swapchain, render pass, image views, and framebuffers.
/// Owned by VulkanRenderer; the render pass is created once and reused
/// across resizes so that the pipeline (which references it) stays valid.
class SwapchainManager {
public:
    /// Create or recreate swapchain + render pass + framebuffers.
    /// Render pass is created only on first call.
    /// @param desired_w  Desired swapchain width (from caller / widget size)
    /// @param desired_h  Desired swapchain height
    /// @return true on success
    bool create(void* physical_device, void* device, void* surface,
                int queue_family, int desired_w, int desired_h);

    /// Acquire the next swapchain image.
    /// @return image index, or ~0u on failure
    uint32_t acquire(void* device, void* semaphore, void* fence = nullptr);

    /// Destroy all resources.
    void release(void* device);

    // Accessors
    int width() const { return w_; }
    int height() const { return h_; }
    void* swapchain() const { return swapchain_; }
    void* renderPass() const { return render_pass_; }
    void* framebuffer(uint32_t idx) const {
        return idx < framebuffers_.size() ? framebuffers_[idx] : nullptr;
    }
    uint32_t imageCount() const {
        return (uint32_t)swapchain_images_.size();
    }

private:
    void* swapchain_ = nullptr;
    void* render_pass_ = nullptr;
    std::vector<void*> swapchain_images_;
    std::vector<void*> swapchain_image_views_;
    std::vector<void*> framebuffers_;
    int w_ = 0, h_ = 0;
};

}  // namespace vsr
