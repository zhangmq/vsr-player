/// Minimal Vulkan renderer for video frames.
/// Delegates instance/device/surface to VulkanContext and
/// swapchain/render-pass/framebuffer to SwapchainManager.

#include "VulkanRenderer.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

// Generated SPIR-V headers (by Makefile: glslc + xxd)
#include "video_vert_spv.h"
#include "video_frag_spv.h"

namespace vsr {

// ── Lifecycle ───────────────────────────────────────────────────────

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { release(); }

// ── Init ────────────────────────────────────────────────────────────

bool VulkanRenderer::init(void* native_window, void* native_display) {
    if (!ctx_.init(native_window, native_display))
        return false;

    VkDevice dev = (VkDevice)ctx_.device();

    // ---- Descriptor set layout ----
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &b;
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl);
    descriptor_set_layout_ = dsl;

    // ---- Descriptor pool ----
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpci, nullptr, &dp);
    descriptor_pool_ = dp;

    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dp;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = (VkDescriptorSetLayout*)&dsl;
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(dev, &dsai, &ds);
    descriptor_set_ = ds;

    printf("VulkanRenderer: initialized\n");
    return true;
}

// ── Swapchain + pipeline ────────────────────────────────────────────

bool VulkanRenderer::create_swapchain_and_pipeline(int w, int h) {
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev) return false;

    if (!swapchain_.create(ctx_.physicalDevice(), ctx_.device(),
                           ctx_.surface(), ctx_.queueFamily(), w, h))
        return false;

    swapchain_width_ = swapchain_.width();
    swapchain_height_ = swapchain_.height();

    // Create pipeline once (lazy) — the render pass is kept alive across
    // resizes, so the pipeline handle stays valid.
    if (pipeline_) return true;

    // Shader modules
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = video_vert_spv_len;
    smci.pCode = (const uint32_t*)video_vert_spv;
    VkShaderModule vs;
    vkCreateShaderModule(dev, &smci, nullptr, &vs);
    smci.codeSize = video_frag_spv_len;
    smci.pCode = (const uint32_t*)video_frag_spv;
    VkShaderModule fs;
    vkCreateShaderModule(dev, &smci, nullptr, &fs);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {0, 0, (float)swapchain_width_, (float)swapchain_height_, 0, 1};
    VkRect2D scissor = {{0, 0}, {(uint32_t)swapchain_width_, (uint32_t)swapchain_height_}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount = 1; vps.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = (VkDescriptorSetLayout*)&descriptor_set_layout_;
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, nullptr, &pl);
    pipeline_layout_ = pl;

    // Dynamic viewport + scissor (needed for letterboxing)
    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = stages;
    pci.pVertexInputState = &vis; pci.pInputAssemblyState = &ias;
    pci.pViewportState = &vps; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi;
    pci.layout = pl;
    pci.renderPass = (VkRenderPass)swapchain_.renderPass();
    pci.subpass = 0;

    VkPipeline pipe;
    vkCreateGraphicsPipelines(dev, nullptr, 1, &pci, nullptr, &pipe);
    pipeline_ = pipe;

    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);

    printf("VulkanRenderer: pipeline created\n");
    return true;
}

// ── Texture update ──────────────────────────────────────────────────

bool VulkanRenderer::update_texture(const uint8_t* data, int w, int h, int bpp) {
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev) return false;

    // Destroy old texture if size changed
    if (texture_ && (w != tex_width_ || h != tex_height_)) {
        vkFreeMemory(dev, (VkDeviceMemory)texture_memory_, nullptr);
        vkDestroyImage(dev, (VkImage)texture_, nullptr);
        texture_ = nullptr; texture_memory_ = nullptr;
    }

    if (!texture_) {
        VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)w, (uint32_t)h, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        VkImage img;
        vkCreateImage(dev, &ici, nullptr, &img);

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, img, &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_memory_type(ctx_.physicalDevice(),
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory mem;
        vkAllocateMemory(dev, &mai, nullptr, &mem);
        vkBindImageMemory(dev, img, mem, 0);

        texture_ = img; texture_memory_ = mem;
        tex_width_ = w; tex_height_ = h;

        // Query actual row pitch (LINEAR tiling may require padding)
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout srl;
        vkGetImageSubresourceLayout(dev, img, &sub, &srl);
        tex_row_pitch_ = srl.rowPitch;
        fprintf(stderr, "VulkanRenderer: texture %dx%d rowPitch=%zu (w*4=%d)\n",
                w, h, tex_row_pitch_, w * 4);

        VkFence f = (VkFence)ctx_.fence();
        vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &f);
        VkCommandBuffer cb = (VkCommandBuffer)ctx_.commandBuffer();
        vkResetCommandBuffer(cb, 0);

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cb);

        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit((VkQueue)ctx_.queue(), 1, &si, f);
        vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
    }

    // Upload data to linear-tiled image, respecting actual row pitch
    void* mapped;
    vkMapMemory(dev, (VkDeviceMemory)texture_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
    uint8_t* dst = (uint8_t*)mapped;
    size_t row_pitch = tex_row_pitch_;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int si = (y * w + x) * bpp;
            size_t di = y * row_pitch + (size_t)x * 4;
            if (bpp == 4) {
                memcpy(&dst[di], &data[si], 4);
            } else {
                dst[di + 0] = data[si + 0];
                dst[di + 1] = data[si + 1];
                dst[di + 2] = data[si + 2];
                dst[di + 3] = 255;
            }
        }
    }
    vkUnmapMemory(dev, (VkDeviceMemory)texture_memory_);

    // Update descriptor set with image view for this texture
    VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = (VkImage)texture_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;

    static VkImageView tex_view = nullptr;
    if (tex_view) vkDestroyImageView(dev, tex_view, nullptr);
    vkCreateImageView(dev, &ivci, nullptr, &tex_view);

    VkDescriptorImageInfo dii = {};
    dii.sampler = (VkSampler)ctx_.sampler();
    dii.imageView = tex_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = (VkDescriptorSet)descriptor_set_;
    wds.dstBinding = 0;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.descriptorCount = 1;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

    return true;
}

// ── Resize ──────────────────────────────────────────────────────────

bool VulkanRenderer::resize(int surface_w, int surface_h) {
    if (swapchain_width_ == surface_w && swapchain_height_ == surface_h)
        return true;
    return create_swapchain_and_pipeline(surface_w, surface_h);
}

// ── Render frame ────────────────────────────────────────────────────

bool VulkanRenderer::render_frame(const uint8_t* data, int video_w, int video_h, bool is_rgba) {
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev || !ctx_.surface() || !swapchain_.swapchain()) return false;

    if (!update_texture(data, video_w, video_h, is_rgba ? 4 : 3)) return false;

    // Calculate letterboxed viewport: fit video in swapchain, keep aspect ratio
    float scale_w = (float)swapchain_width_  / (float)video_w;
    float scale_h = (float)swapchain_height_ / (float)video_h;
    float scale   = (scale_w < scale_h) ? scale_w : scale_h;

    int vp_w = (int)(video_w * scale);
    int vp_h = (int)(video_h * scale);
    int vp_x = (swapchain_width_  - vp_w) / 2;
    int vp_y = (swapchain_height_ - vp_h) / 2;

    // Acquire swapchain image
    uint32_t idx = swapchain_.acquire(dev, ctx_.imageAvailableSemaphore());
    if (idx == ~0u) return false;

    // Record + submit
    VkFence f = (VkFence)ctx_.fence();
    VkCommandBuffer cb = (VkCommandBuffer)ctx_.commandBuffer();
    vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &f);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue cv = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = (VkRenderPass)swapchain_.renderPass();
    rpbi.framebuffer = (VkFramebuffer)swapchain_.framebuffer(idx);
    rpbi.renderArea = {{0, 0}, {(uint32_t)swapchain_width_, (uint32_t)swapchain_height_}};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;

    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport + scissor for letterboxed video area
    VkViewport vp = {(float)vp_x, (float)vp_y, (float)vp_w, (float)vp_h, 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D scissor = {{(int32_t)vp_x, (int32_t)vp_y}, {(uint32_t)vp_w, (uint32_t)vp_h}};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        (VkPipelineLayout)pipeline_layout_, 0, 1,
        (VkDescriptorSet*)&descriptor_set_, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = (const VkSemaphore*)ctx_.imageAvailableSemaphore();
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = (const VkSemaphore*)ctx_.renderFinishedSemaphore();
    vkQueueSubmit((VkQueue)ctx_.queue(), 1, &si, f);

    VkSwapchainKHR sc = (VkSwapchainKHR)swapchain_.swapchain();
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = (const VkSemaphore*)ctx_.renderFinishedSemaphore();
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR((VkQueue)ctx_.queue(), &pi);

    return true;
}

// ── Release ─────────────────────────────────────────────────────────

void VulkanRenderer::release() {
    VkDevice dev = (VkDevice)ctx_.device();
    if (!dev) return;
    vkDeviceWaitIdle(dev);

    // Destroy own resources
    if (texture_) {
        // Destroy the static tex_view too if needed (best effort)
        vkDestroyImage(dev, (VkImage)texture_, nullptr);
        texture_ = nullptr;
    }
    if (texture_memory_) {
        vkFreeMemory(dev, (VkDeviceMemory)texture_memory_, nullptr);
        texture_memory_ = nullptr;
    }
    if (pipeline_) { vkDestroyPipeline(dev, (VkPipeline)pipeline_, nullptr); pipeline_ = nullptr; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(dev, (VkPipelineLayout)pipeline_layout_, nullptr); pipeline_layout_ = nullptr; }
    if (descriptor_pool_) { vkDestroyDescriptorPool(dev, (VkDescriptorPool)descriptor_pool_, nullptr); descriptor_pool_ = nullptr; }
    if (descriptor_set_layout_) { vkDestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout)descriptor_set_layout_, nullptr); descriptor_set_layout_ = nullptr; }
    descriptor_set_ = nullptr;

    // Destroy swapchain resources (render pass, swapchain, framebuffers)
    swapchain_.release(dev);

    // Destroy Vulkan context (device, surface, instance, sync, etc.)
    ctx_.release();

    swapchain_width_ = swapchain_height_ = 0;

    printf("VulkanRenderer: released\n");
}

}  // namespace vsr
