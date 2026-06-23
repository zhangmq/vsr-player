#include "SwapchainManager.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

namespace vsr {

// ── Create / Recreate ───────────────────────────────────────────────

bool SwapchainManager::create(void* physical_device, void* device,
                              void* surface, int queue_family,
                              int desired_w, int desired_h) {
    VkDevice dev = (VkDevice)device;
    (void)queue_family;
    VkPhysicalDevice pd = (VkPhysicalDevice)physical_device;
    if (!dev) return false;

    // Destroy old swapchain resources (keep render pass alive)
    for (auto* fb : framebuffers_)
        vkDestroyFramebuffer(dev, (VkFramebuffer)fb, nullptr);
    for (auto* iv : swapchain_image_views_)
        vkDestroyImageView(dev, (VkImageView)iv, nullptr);
    if (swapchain_) {
        vkDestroySwapchainKHR(dev, (VkSwapchainKHR)swapchain_, nullptr);
        swapchain_ = nullptr;
    }
    framebuffers_.clear();
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    // Determine swapchain extent from surface capabilities.
    // Wayland may report 0xFFFFFFFF as a "you choose" sentinel.
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, (VkSurfaceKHR)surface, &caps);

    uint32_t sw, sh;
    if (caps.currentExtent.width == 0xFFFFFFFF) {
        // Wayland "you choose" — use caller's desired dimensions
        sw = (uint32_t)desired_w;
        sh = (uint32_t)desired_h;
    } else {
        sw = caps.currentExtent.width;
        sh = caps.currentExtent.height;
    }
    // Clamp to surface capabilities
    sw = std::max(sw, caps.minImageExtent.width);
    sw = std::min(sw, caps.maxImageExtent.width);
    sh = std::max(sh, caps.minImageExtent.height);
    sh = std::min(sh, caps.maxImageExtent.height);
    w_ = (int)sw;
    h_ = (int)sh;

    // ---- Swapchain ----
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = (VkSurfaceKHR)surface;
    sci.minImageCount = 2;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {sw, sh};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sci, nullptr, &sc) != VK_SUCCESS) {
        fprintf(stderr, "Swapchain: vkCreateSwapchainKHR failed\n");
        return false;
    }
    swapchain_ = sc;

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(dev, sc, &ic, nullptr);
    std::vector<VkImage> sc_images(ic);
    vkGetSwapchainImagesKHR(dev, sc, &ic, sc_images.data());

    // ---- Render pass (created once) ----
    if (!render_pass_) {
        VkAttachmentDescription ad = {};
        ad.format = sci.imageFormat;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp = {};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ar;

        VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &ad;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;

        VkRenderPass rp;
        if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) {
            fprintf(stderr, "Swapchain: vkCreateRenderPass failed\n");
            return false;
        }
        render_pass_ = rp;
    }

    // ---- Image views + Framebuffers ----
    swapchain_images_.resize(ic);
    swapchain_image_views_.resize(ic);
    framebuffers_.resize(ic);
    for (uint32_t i = 0; i < ic; i++) {
        swapchain_images_[i] = sc_images[i];

        VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = sc_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = sci.imageFormat;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;

        VkImageView iv;
        if (vkCreateImageView(dev, &ivci, nullptr, &iv) != VK_SUCCESS) {
            fprintf(stderr, "Swapchain: vkCreateImageView failed\n");
            return false;
        }
        swapchain_image_views_[i] = iv;

        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = (VkRenderPass)render_pass_;
        fci.attachmentCount = 1;
        fci.pAttachments = (VkImageView*)&swapchain_image_views_[i];
        fci.width = sw; fci.height = sh; fci.layers = 1;

        VkFramebuffer fb;
        if (vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) {
            fprintf(stderr, "Swapchain: vkCreateFramebuffer failed\n");
            return false;
        }
        framebuffers_[i] = fb;
    }

    printf("Vulkan: swapchain %dx%d, %u images\n", w_, h_, ic);
    return true;
}

// ── Acquire next image ──────────────────────────────────────────────

uint32_t SwapchainManager::acquire(void* device, void* semaphore,
                                    void* fence) {
    uint32_t idx;
    VkResult res = vkAcquireNextImageKHR(
        (VkDevice)device, (VkSwapchainKHR)swapchain_,
        100'000'000,  // 100ms — finite to avoid permanent UI freeze
        semaphore ? (VkSemaphore)semaphore : VK_NULL_HANDLE,
        fence ? (VkFence)fence : VK_NULL_HANDLE,
        &idx);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Swapchain: acquire failed (%d), recreating\n", res);
        return ~0u;
    }
    return idx;
}

// ── Release ─────────────────────────────────────────────────────────

void SwapchainManager::release(void* device) {
    VkDevice dev = (VkDevice)device;
    if (!dev) return;

    for (auto* fb : framebuffers_)
        vkDestroyFramebuffer(dev, (VkFramebuffer)fb, nullptr);
    for (auto* iv : swapchain_image_views_)
        vkDestroyImageView(dev, (VkImageView)iv, nullptr);
    framebuffers_.clear();
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    if (render_pass_) {
        vkDestroyRenderPass(dev, (VkRenderPass)render_pass_, nullptr);
        render_pass_ = nullptr;
    }
    if (swapchain_) {
        vkDestroySwapchainKHR(dev, (VkSwapchainKHR)swapchain_, nullptr);
        swapchain_ = nullptr;
    }
    w_ = h_ = 0;
}

}  // namespace vsr
