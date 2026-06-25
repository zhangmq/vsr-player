/// VulkanRenderer — thin coordinator for two VideoPipeline instances.
///
/// Texture mutation happens on the worker thread, protected by a Render Gate
/// (lock-free worker↔render handshake). The gate ensures the render thread
/// isn't recording commands referencing old textures before they're destroyed.

#include "VulkanRenderer.h"
#include "../api/Player.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ── Logging helpers ────────────────────────────────────────────────

static int rctr = 0;
#define RLOG(fmt, ...) \
    fprintf(stderr, "[render] #%d " fmt "\n", ++rctr, ##__VA_ARGS__)

#define RSNAP(label, gate, ready, in_frame, frames_since)  \
    RLOG("snap:%s gate=%d ready=%d in_frame=%d frames_since=%d", \
         label, (int)(gate), (int)(ready), (int)(in_frame), (int)(frames_since))

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

namespace vsr {

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() {}

// ── Pipeline init (once, from Player::initialize) ──────────────────

bool VulkanRenderer::init_pipelines(
        IVulkanContext& vk,
        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
        const uint32_t* vertSpv, size_t vertSpvLen) {

    VkDevice dev = (VkDevice)vk.vkDevice();
    VkRenderPass renderPass = (VkRenderPass)vk.vkRenderPass();
    if (!dev || !renderPass) return false;

    cached_device_ = dev;
    pipelines_ready_ = false;

    {
        PipelineConfig cfg;
        cfg.textures = {{0, VK_FORMAT_R8G8B8A8_UNORM, 0, 0}};
        cfg.vertSpv = vertSpv; cfg.vertSpvLen = vertSpvLen;
        cfg.fragSpv = rgbaFragSpv; cfg.fragSpvLen = rgbaFragSpvLen;
        if (!rgbaPipeline_.init_pipeline(vk, renderPass, cfg)) {
            fprintf(stderr, "VulkanRenderer: rgbaPipeline init_pipeline failed\n");
            return false;
        }
    }

    {
        PipelineConfig cfg;
        cfg.textures = {
            {0, VK_FORMAT_R8_UNORM, 0, 0},
            {1, VK_FORMAT_R8G8_UNORM, 0, 0}};
        cfg.vertSpv = vertSpv; cfg.vertSpvLen = vertSpvLen;
        cfg.fragSpv = nv12FragSpv; cfg.fragSpvLen = nv12FragSpvLen;
        if (!nv12Pipeline_.init_pipeline(vk, renderPass, cfg)) {
            fprintf(stderr, "VulkanRenderer: nv12Pipeline init_pipeline failed\n");
            return false;
        }
    }

    printf("VulkanRenderer: pipelines initialized (no textures yet)\n");
    return true;
}

// ── Record to external command buffer (render thread) ──────────────

void VulkanRenderer::record_to_cb(void* cb, int extentW, int extentH,
                                   Path path) {
    VkCommandBuffer cmdBuf = (VkCommandBuffer)cb;
    if (!cmdBuf) { RLOG("no-cmdBuf"); return; }

    if (mutation_gate_.load(std::memory_order_acquire) != 0) {
        static int gate_skip = 0;
        if (++gate_skip % 180 == 1)
            RLOG("gate-skip gate=%d ready=%d", mutation_gate_.load(), pipelines_ready_);
        return;
    }
    if (!pipelines_ready_) {
        static int ready_skip = 0;
        if (++ready_skip % 180 == 1)
            RLOG("ready-skip");
        return;
    }

    // Enter critical section.
    render_in_frame_.fetch_add(1, std::memory_order_acquire);

    // Double-check: worker might have started mutation between the
    // initial loads and the fetch_add above.
    if (mutation_gate_.load(std::memory_order_acquire) != 0 ||
        !pipelines_ready_) {
        render_in_frame_.fetch_sub(1, std::memory_order_release);
        return;
    }

    // Layout transitions (first frame after reconfig).
    if (textures_need_transition_ && cached_device_) {
        rgbaPipeline_.ensureTransitioned(cached_device_, cmdBuf);
        nv12Pipeline_.ensureTransitioned(cached_device_, cmdBuf);
        textures_need_transition_ = false;
    }

    int srcW = video_w_, srcH = video_h_;
    if (path == Path::VSR) { srcW *= vsr_scale_; srcH *= vsr_scale_; }
    float scaleW = (float)extentW / srcW, scaleH = (float)extentH / srcH;
    float sc = (scaleW < scaleH) ? scaleW : scaleH;
    int vpW = (int)(srcW * sc), vpH = (int)(srcH * sc);
    int vpX = (extentW - vpW) / 2, vpY = (extentH - vpH) / 2;

    VkViewport vp = {(float)vpX, (float)vpY, (float)vpW, (float)vpH, 0.0f, 1.0f};
    vkCmdSetViewport(cmdBuf, 0, 1, &vp);
    VkRect2D scissor = {{(int32_t)vpX, (int32_t)vpY}, {(uint32_t)vpW, (uint32_t)vpH}};
    vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

    if (path == Path::VSR)
        rgbaPipeline_.bind(cmdBuf);
    else
        nv12Pipeline_.bind(cmdBuf);

    frames_since_mutation_.fetch_add(1, std::memory_order_release);

    static int rok = 0;
    if (++rok % 180 == 1)
        RLOG("draw-OK #%d %s src=%dx%d scaled=%dx%d dst=%dx%d",
             rok, path == Path::VSR ? "VSR" : "NV12",
             video_w_, video_h_,
             video_w_ * vsr_scale_, video_h_ * vsr_scale_,
             extentW, extentH);

    render_in_frame_.fetch_sub(1, std::memory_order_release);
}

// ── Resize ─────────────────────────────────────────────────────────

bool VulkanRenderer::resize(int w, int h) {
    last_widget_w_ = w;
    last_widget_h_ = h;
    return true;
}

// ── Release (main thread, after worker stopped) ────────────────────

void VulkanRenderer::release(IVulkanContext& vk) {
    VkDevice dev = (VkDevice)vk.vkDevice();
    if (!dev) return;

    pipelines_ready_ = false;
    vk.waitIdle();

    rgbaPipeline_.release(dev);
    nv12Pipeline_.release(dev);

    video_w_ = video_h_ = 0;
    vsr_scale_ = 1;

    printf("VulkanRenderer: released\n");
}

// ── Render Gate ────────────────────────────────────────────────────

void VulkanRenderer::begin_mutation(IVulkanContext& vk) {
    RSNAP("bm-enter", mutation_gate_.load(), pipelines_ready_,
          render_in_frame_.load(), frames_since_mutation_.load());

    if (frames_since_mutation_.load(std::memory_order_acquire) >= 2) {
        RLOG("destroy-retired frames=%d", frames_since_mutation_.load());
        rgbaPipeline_.destroy_retired();
        nv12Pipeline_.destroy_retired();
    }

    mutation_gate_.store(1, std::memory_order_release);
    pipelines_ready_ = false;

    while (render_in_frame_.load(std::memory_order_acquire) > 0) {
        if (running_flag_ && !running_flag_->load(std::memory_order_acquire))
            return;
        std::this_thread::yield();
    }
    RLOG("spin-done, waitIdle...");
    vk.waitIdle();
    RLOG("waitIdle-done");
}

void VulkanRenderer::end_mutation() {
    RSNAP("bm-exit", mutation_gate_.load(), pipelines_ready_,
          render_in_frame_.load(), frames_since_mutation_.load());
    pipelines_ready_ = true;
    mutation_gate_.store(0, std::memory_order_release);
    frames_since_mutation_.store(0, std::memory_order_release);
}

// ── Reconfigure scale (RGBA texture only) ─────────────────────────

bool VulkanRenderer::reconfigure_scale(
        int videoW, int videoH, int newScale, IVulkanContext& vk) {
    if (!rgbaPipeline_.ready()) return false;

    begin_mutation(vk);

    video_w_ = videoW; video_h_ = videoH; vsr_scale_ = newScale;

    int vsrW = videoW * newScale;
    int vsrH = videoH * newScale;

    std::vector<TextureBinding> newRgba = {
        {0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)vsrW, (uint32_t)vsrH}};
    if (!rgbaPipeline_.reconfigure_textures(vk, newRgba)) {
        fprintf(stderr, "VulkanRenderer: reconfigure_scale failed\n");
        end_mutation();
        return false;
    }

    textures_need_transition_ = true;
    end_mutation();

    fprintf(stderr, "VulkanRenderer: scale reconfigured to %d (RGBA %dx%d)\n",
            newScale, vsrW, vsrH);
    return true;
}

// ── Reconfigure all textures (video switch) ───────────────────────

bool VulkanRenderer::reconfigure_all_textures(
        int videoW, int videoH, int scale, IVulkanContext& vk) {
    if (!cached_device_) return false;

    begin_mutation(vk);

    video_w_ = videoW; video_h_ = videoH; vsr_scale_ = scale;

    int vsrW = videoW * scale;
    int vsrH = videoH * scale;

    {
        std::vector<TextureBinding> newRgba = {
            {0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)vsrW, (uint32_t)vsrH}};
        if (!rgbaPipeline_.reconfigure_textures(vk, newRgba)) {
            fprintf(stderr, "VulkanRenderer: rgba reconfigure failed\n");
            end_mutation();
            return false;
        }
    }

    {
        std::vector<TextureBinding> newNv12 = {
            {0, VK_FORMAT_R8_UNORM, (uint32_t)videoW, (uint32_t)videoH},
            {1, VK_FORMAT_R8G8_UNORM, (uint32_t)(videoW/2), (uint32_t)(videoH/2)}};
        if (!nv12Pipeline_.reconfigure_textures(vk, newNv12)) {
            fprintf(stderr, "VulkanRenderer: nv12 reconfigure failed\n");
            end_mutation();
            return false;
        }
    }

    textures_need_transition_ = true;
    end_mutation();

    fprintf(stderr, "VulkanRenderer: all textures reconfigured "
            "(video %dx%d scale %d)\n", videoW, videoH, scale);
    return true;
}

}  // namespace vsr
