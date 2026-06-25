/// VideoPipeline — config-driven Vulkan graphics pipeline with InteropTextures.
///
/// Lifecycle:
///   1. Client calls init_pipeline() once — creates shaders, layout, sampler, pipeline.
///   2. Core calls reconfigure_textures() per LOAD_FILE / RESIZE — creates InteropTextures,
///      allocates descriptor set (first call) or updates it (subsequent calls).
///
/// This split keeps Vulkan resource creation in the client, while letting the core
/// drive texture configuration when video dimensions are known.

#include "VideoPipeline.h"
#include "../api/Player.h"  // IVulkanContext

#include <cstdio>
#include <vulkan/vulkan.h>

namespace vsr {

// ── Lifecycle ──────────────────────────────────────────────────────

VideoPipeline::VideoPipeline() = default;

VideoPipeline::~VideoPipeline() {
    // Resources are cleared in release(), which must be called before
    // the VkDevice is destroyed.
}

// ── Init pipeline (client, once) ───────────────────────────────────

bool VideoPipeline::init_pipeline(IVulkanContext& vk, void* renderPass,
                                   const PipelineConfig& cfg) {
    VkDevice dev = (VkDevice)vk.vkDevice();
    VkResult res;

    if (cfg.textures.empty()) {
        fprintf(stderr, "VideoPipeline: no textures in config\n");
        return false;
    }

    // ── 1. Create sampler ──
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = 0.0f;
    sci.maxLod = 0.0f;

    VkSampler sampler;
    res = vkCreateSampler(dev, &sci, nullptr, &sampler);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateSampler failed (%d)\n", res);
        release(dev);
        return false;
    }
    sampler_ = (void*)sampler;

    // ── 2. Create descriptor set layout ──
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(cfg.textures.size());
    for (auto& tb : cfg.textures) {
        VkDescriptorSetLayoutBinding b = {};
        b.binding = tb.binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo dslci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = (uint32_t)bindings.size();
    dslci.pBindings = bindings.data();

    VkDescriptorSetLayout dsLayout;
    res = vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsLayout);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateDescriptorSetLayout failed (%d)\n",
                res);
        release(dev);
        return false;
    }
    descriptor_set_layout_ = (void*)dsLayout;

    // ── 3. Create descriptor pool ──
    VkDescriptorPoolSize ps = {};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = (uint32_t)cfg.textures.size();

    VkDescriptorPoolCreateInfo dpci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;

    VkDescriptorPool dPool;
    res = vkCreateDescriptorPool(dev, &dpci, nullptr, &dPool);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateDescriptorPool failed (%d)\n", res);
        release(dev);
        return false;
    }
    descriptor_pool_ = (void*)dPool;

    // ── 4. Create shader modules ──
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = cfg.vertSpvLen;
    smci.pCode = cfg.vertSpv;

    VkShaderModule vertMod;
    res = vkCreateShaderModule(dev, &smci, nullptr, &vertMod);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateShaderModule (vert) failed (%d)\n", res);
        release(dev);
        return false;
    }
    vert_module_ = (void*)vertMod;

    smci.codeSize = cfg.fragSpvLen;
    smci.pCode = cfg.fragSpv;

    VkShaderModule fragMod;
    res = vkCreateShaderModule(dev, &smci, nullptr, &fragMod);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateShaderModule (frag) failed (%d)\n", res);
        release(dev);
        return false;
    }
    frag_module_ = (void*)fragMod;

    // ── 5. Create pipeline layout ──
    VkPipelineLayoutCreateInfo plci = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsLayout;

    VkPipelineLayout pl;
    res = vkCreatePipelineLayout(dev, &plci, nullptr, &pl);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreatePipelineLayout failed (%d)\n", res);
        release(dev);
        return false;
    }
    pipeline_layout_ = (void*)pl;

    // ── 6. Create graphics pipeline ──
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vis = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo ias = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {0, 0, 1920, 1080, 0, 1};
    VkRect2D sc = {{0, 0}, {1920, 1080}};
    VkPipelineViewportStateCreateInfo vps = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo cbs = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vis;
    pci.pInputAssemblyState = &ias;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi;
    pci.layout = pl;
    pci.renderPass = (VkRenderPass)renderPass;
    pci.subpass = 0;

    VkPipeline pipeline;
    res = vkCreateGraphicsPipelines(dev, nullptr, 1, &pci, nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "VideoPipeline: vkCreateGraphicsPipelines failed (%d)\n", res);
        release(dev);
        return false;
    }
    pipeline_ = (void*)pipeline;

    fprintf(stderr, "VideoPipeline: pipeline ready (%zu descriptors)\n",
            cfg.textures.size());
    return true;
}

// ── Layout transition (called from render thread via record_to_cb) ──

void VideoPipeline::ensureTransitioned(void* device, void* commandBuffer) {
    VkCommandBuffer cb = (VkCommandBuffer)commandBuffer;
    for (auto& tex : interopTextures_) {
        tex.transitionLayout(device, cb,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// ── Bind ───────────────────────────────────────────────────────────

void VideoPipeline::bind(void* commandBuffer) {
    VkCommandBuffer cb = (VkCommandBuffer)commandBuffer;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      (VkPipeline)pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            (VkPipelineLayout)pipeline_layout_,
                            0, 1, (VkDescriptorSet*)&descriptor_set_,
                            0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ── InteropTexture access ──────────────────────────────────────────

InteropTexture& VideoPipeline::interopTexture(uint32_t idx) {
    return interopTextures_[idx];
}

// ── Release ────────────────────────────────────────────────────────

void VideoPipeline::release(void* device) {
    VkDevice dev = (VkDevice)device;

    if (pipeline_) {
        vkDestroyPipeline(dev, (VkPipeline)pipeline_, nullptr);
        pipeline_ = nullptr;
    }
    if (pipeline_layout_) {
        vkDestroyPipelineLayout(dev, (VkPipelineLayout)pipeline_layout_, nullptr);
        pipeline_layout_ = nullptr;
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(dev, (VkDescriptorPool)descriptor_pool_, nullptr);
        descriptor_pool_ = nullptr;
    }
    if (descriptor_set_layout_) {
        vkDestroyDescriptorSetLayout(
            dev, (VkDescriptorSetLayout)descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = nullptr;
    }
    if (vert_module_) {
        vkDestroyShaderModule(dev, (VkShaderModule)vert_module_, nullptr);
        vert_module_ = nullptr;
    }
    if (frag_module_) {
        vkDestroyShaderModule(dev, (VkShaderModule)frag_module_, nullptr);
        frag_module_ = nullptr;
    }
    if (sampler_) {
        vkDestroySampler(dev, (VkSampler)sampler_, nullptr);
        sampler_ = nullptr;
    }

    interopTextures_.clear();
    retired_textures_.clear();

    descriptor_set_ = nullptr;
}

void VideoPipeline::destroy_retired() {
    retired_textures_.clear();
}

// ── Reconfigure textures ───────────────────────────────────────────

bool VideoPipeline::reconfigure_textures(
        IVulkanContext& vk,
        const std::vector<TextureBinding>& new_textures) {
    VkDevice dev = (VkDevice)vk.vkDevice();
    VkPhysicalDevice pd = (VkPhysicalDevice)vk.vkPhysicalDevice();

    if (!dev || !descriptor_pool_ || new_textures.empty()) return false;

    // 1. Move old InteropTextures to retired list (don't destroy yet)
    for (auto& tex : interopTextures_) {
        if (tex.valid())
            retired_textures_.push_back(std::move(tex));
    }
    interopTextures_.clear();

    // 2. Create new InteropTextures at new sizes
    interopTextures_.reserve(new_textures.size());
    for (auto& tb : new_textures) {
        interopTextures_.emplace_back();
        if (!interopTextures_.back().init(
                (VkDevice_T*)dev, (VkPhysicalDevice_T*)pd,
                tb.width, tb.height, tb.format)) {
            fprintf(stderr, "VideoPipeline: reconfigure_textures init failed\n");
            interopTextures_.clear();
            return false;
        }
    }

    // 3. Allocate descriptor set on first call
    if (!descriptor_set_) {
        VkDescriptorSetLayout dsLayout = (VkDescriptorSetLayout)descriptor_set_layout_;
        VkDescriptorSetAllocateInfo dsai = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = (VkDescriptorPool)descriptor_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsLayout;

        VkDescriptorSet dSet;
        VkResult res = vkAllocateDescriptorSets(dev, &dsai, &dSet);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "VideoPipeline: vkAllocateDescriptorSets failed (%d)\n", res);
            interopTextures_.clear();
            return false;
        }
        descriptor_set_ = (void*)dSet;
    }

    // 4. Update descriptor set with new image views (sampler unchanged)
    std::vector<VkDescriptorImageInfo> imageInfos(new_textures.size());
    std::vector<VkWriteDescriptorSet> writes(new_textures.size());
    for (size_t i = 0; i < new_textures.size(); i++) {
        imageInfos[i] = {};
        imageInfos[i].sampler = (VkSampler)sampler_;
        imageInfos[i].imageView =
            (VkImageView)interopTextures_[i].imageView();
        imageInfos[i].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = (VkDescriptorSet)descriptor_set_;
        writes[i].dstBinding = new_textures[i].binding;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(),
                           0, nullptr);

    fprintf(stderr, "VideoPipeline: reconfigured %zu texture(s)\n",
            new_textures.size());
    return true;
}

}  // namespace vsr
