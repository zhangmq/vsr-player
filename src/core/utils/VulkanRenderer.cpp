/// VulkanRenderer — thin coordinator that delegates to VulkanContext,
/// SwapchainManager, and two VideoPipeline instances (RGBA + NV12).

#include "VulkanRenderer.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

namespace vsr {

// ── Lifecycle ───────────────────────────────────────────────────────

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { release(); }

// ── Init ────────────────────────────────────────────────────────────

bool VulkanRenderer::init(void* native_window, void* native_display) {
    if (!ctx_.init(native_window, native_display))
        return false;
    printf("VulkanRenderer: initialized\n");
    return true;
}

// ── Init pipelines ──────────────────────────────────────────────────

bool VulkanRenderer::init_pipelines(int videoW, int videoH, int scale,
                                     int widgetW, int widgetH,
                                     const uint32_t* rgbaFragSpv,
                                     size_t rgbaFragSpvLen,
                                     const uint32_t* nv12FragSpv,
                                     size_t nv12FragSpvLen,
                                     const uint32_t* vertSpv,
                                     size_t vertSpvLen) {
    VkDevice dev = (VkDevice)ctx_.device();
    VkPhysicalDevice pd = (VkPhysicalDevice)ctx_.physicalDevice();
    if (!dev) return false;

    // Save SPIR-V + params for pipeline recreation on resize
    saved_vert_spv_ = vertSpv;
    saved_rgba_frag_spv_ = rgbaFragSpv;
    saved_nv12_frag_spv_ = nv12FragSpv;
    saved_vert_len_ = vertSpvLen;
    saved_rgba_frag_len_ = rgbaFragSpvLen;
    saved_nv12_frag_len_ = nv12FragSpvLen;

    video_w_ = videoW;
    video_h_ = videoH;
    vsr_scale_ = scale;

    int vsrW = videoW * scale;
    int vsrH = videoH * scale;

    // Create swapchain if it doesn't exist yet, using widget pixel dimensions
    if (!swapchain_.swapchain()) {
        int sw_w = widgetW > 0 ? widgetW : 1280;
        int sw_h = widgetH > 0 ? widgetH : 720;
        if (!swapchain_.create(pd, dev, ctx_.surface(), ctx_.queueFamily(),
                                sw_w, sw_h)) {
            fprintf(stderr, "VulkanRenderer: swapchain create failed\n");
            return false;
        }
    }

    // Release old pipelines (if any) before recreating
    rgbaPipeline_.release(dev);
    nv12Pipeline_.release(dev);

    VkRenderPass renderPass = (VkRenderPass)swapchain_.renderPass();
    VkQueue queue = (VkQueue)ctx_.queue();
    VkCommandPool cmdPool = (VkCommandPool)ctx_.commandPool();

    // ── RGBA pipeline (VSR path) ──
    {
        PipelineConfig cfg;
        cfg.textures = {
            {0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)vsrW, (uint32_t)vsrH}};
        cfg.vertSpv = vertSpv;
        cfg.vertSpvLen = vertSpvLen;
        cfg.fragSpv = rgbaFragSpv;
        cfg.fragSpvLen = rgbaFragSpvLen;

        if (!rgbaPipeline_.init(dev, pd, renderPass, queue, cmdPool, cfg)) {
            fprintf(stderr, "VulkanRenderer: rgbaPipeline init failed\n");
            return false;
        }
    }

    // ── NV12 pipeline (NO-VSR path) ──
    {
        PipelineConfig cfg;
        cfg.textures = {
            {0, VK_FORMAT_R8_UNORM, (uint32_t)videoW, (uint32_t)videoH},
            {1, VK_FORMAT_R8G8_UNORM, (uint32_t)(videoW / 2),
             (uint32_t)(videoH / 2)},
        };
        cfg.vertSpv = vertSpv;
        cfg.vertSpvLen = vertSpvLen;
        cfg.fragSpv = nv12FragSpv;
        cfg.fragSpvLen = nv12FragSpvLen;

        if (!nv12Pipeline_.init(dev, pd, renderPass, queue, cmdPool, cfg)) {
            fprintf(stderr, "VulkanRenderer: nv12Pipeline init failed\n");
            return false;
        }
    }

    pipelines_ready_ = true;
    printf("VulkanRenderer: pipelines initialized (video %dx%d scale %d)\n",
           videoW, videoH, scale);
    return true;
}

// ── Render frame ──────────────────────────────────────────────────────

bool VulkanRenderer::render_frame(Path path) {
    VkDevice dev = (VkDevice)ctx_.device();
    VkQueue queue = (VkQueue)ctx_.queue();
    if (!dev || !ctx_.surface() || !swapchain_.swapchain()) return false;

    // Acquire swapchain image.
    // On failure (timeout or surface mismatch), recreate the swapchain
    // at the last known widget size and retry once.
    uint32_t idx = swapchain_.acquire(dev, ctx_.imageAvailableSemaphore());
    if (idx == ~0u) {
        if (last_widget_w_ > 0 && last_widget_h_ > 0) {
            swapchain_.create(ctx_.physicalDevice(), ctx_.device(),
                              ctx_.surface(), ctx_.queueFamily(),
                              last_widget_w_, last_widget_h_);
            idx = swapchain_.acquire(dev, ctx_.imageAvailableSemaphore());
        }
        if (idx == ~0u) return false;
    }

    // Wait for fence, reset
    VkFence fence = (VkFence)ctx_.fence();
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &fence);

    VkCommandBuffer cb = (VkCommandBuffer)ctx_.commandBuffer();
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);

    // Begin render pass
    VkClearValue cv = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = (VkRenderPass)swapchain_.renderPass();
    rpbi.framebuffer = (VkFramebuffer)swapchain_.framebuffer(idx);
    rpbi.renderArea = {{0, 0},
                       {(uint32_t)swapchain_.width(),
                        (uint32_t)swapchain_.height()}};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // Letterboxing calculation
    int srcW = video_w_, srcH = video_h_;
    if (path == Path::VSR) {
        srcW = video_w_ * vsr_scale_;
        srcH = video_h_ * vsr_scale_;
    }
    int sw = swapchain_.width(), sh = swapchain_.height();
    float scaleW = (float)sw / srcW;
    float scaleH = (float)sh / srcH;
    float scale = (scaleW < scaleH) ? scaleW : scaleH;
    int vpW = (int)(srcW * scale);
    int vpH = (int)(srcH * scale);
    int vpX = (sw - vpW) / 2;
    int vpY = (sh - vpH) / 2;

    // Set viewport + scissor
    VkViewport vp = {(float)vpX, (float)vpY, (float)vpW, (float)vpH,
                     0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor = {{(int32_t)vpX, (int32_t)vpY},
                        {(uint32_t)vpW, (uint32_t)vpH}};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    // Bind correct pipeline and draw
    if (path == Path::VSR) {
        rgbaPipeline_.bind(cb);
    } else {
        nv12Pipeline_.bind(cb);
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    // Submit
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore imgSem = (VkSemaphore)ctx_.imageAvailableSemaphore();
    VkSemaphore doneSem = (VkSemaphore)ctx_.renderFinishedSemaphore();

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imgSem;
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &doneSem;
    vkQueueSubmit(queue, 1, &si, fence);

    // Present
    VkSwapchainKHR sc = (VkSwapchainKHR)swapchain_.swapchain();
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &doneSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(queue, &pi);

    return true;
}

// ── Resize ──────────────────────────────────────────────────────────

bool VulkanRenderer::resize(int w, int h) {
    last_widget_w_ = w;
    last_widget_h_ = h;
    if (swapchain_.width() == w && swapchain_.height() == h)
        return true;
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev) return false;

    // SwapchainManager preserves the render pass (created once), so
    // pipelines remain valid across resize — only framebuffers change.
    return swapchain_.create(ctx_.physicalDevice(), ctx_.device(),
                              ctx_.surface(), ctx_.queueFamily(), w, h);
}

// ── Release ─────────────────────────────────────────────────────────

void VulkanRenderer::release() {
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev) return;
    vkDeviceWaitIdle(dev);

    // 1. Destroy pipelines (while device alive)
    rgbaPipeline_.release(dev);
    nv12Pipeline_.release(dev);

    // 2. Destroy swapchain
    swapchain_.release(dev);

    // 3. Destroy context (device, instance, etc.)
    ctx_.release();

    pipelines_ready_ = false;
    video_w_ = video_h_ = 0;
    vsr_scale_ = 1;

    printf("VulkanRenderer: released\n");
}

// ── Shader storage + deferred pipeline init ──────────────────────────

void VulkanRenderer::set_shader_data(
        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
        const uint32_t* vertSpv, size_t vertSpvLen) {
    saved_vert_spv_ = vertSpv;
    saved_rgba_frag_spv_ = rgbaFragSpv;
    saved_nv12_frag_spv_ = nv12FragSpv;
    saved_vert_len_ = vertSpvLen;
    saved_rgba_frag_len_ = rgbaFragSpvLen;
    saved_nv12_frag_len_ = nv12FragSpvLen;
}

bool VulkanRenderer::init_pipelines_with_saved_spv(
        int videoW, int videoH, int scale, int widgetW, int widgetH) {
    if (!saved_vert_spv_ || !saved_rgba_frag_spv_ || !saved_nv12_frag_spv_) {
        fprintf(stderr, "VulkanRenderer: SPIR-V not set — "
                "call set_shader_data() first\n");
        return false;
    }
    return init_pipelines(videoW, videoH, scale, widgetW, widgetH,
                          saved_rgba_frag_spv_, saved_rgba_frag_len_,
                          saved_nv12_frag_spv_, saved_nv12_frag_len_,
                          saved_vert_spv_, saved_vert_len_);
}

}  // namespace vsr
