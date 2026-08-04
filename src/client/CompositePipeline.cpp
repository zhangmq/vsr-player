#include "CompositePipeline.h"
#include "Log.h"
#include "composite_vert_spv.h"
#include "composite_frag_spv.h">

extern "C" {
VkResult vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
VkResult vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
void vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
void vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*);
void vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*);
void vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*);
}

CompositePipeline::~CompositePipeline() {
    if (!dev_) return;
    if (pipe_)       vkDestroyPipeline(dev_, pipe_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(dev_, pipeLayout_, nullptr);
    if (rp_)         vkDestroyRenderPass(dev_, rp_, nullptr);
}

bool CompositePipeline::create(VkDevice dev, VkDescriptorSetLayout dsLayout) {
    dev_ = dev;

    // Render pass describing Qt's swapchain attachment (B8G8R8A8, load/store).
    // The actual render pass is begun by Qt; this one only pins compatibility.
    VkAttachmentDescription att = {};
    att.format         = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount    = 1; rpci.pSubpasses    = &sub;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp_) != VK_SUCCESS) {
        MLOG_ERR("vkCreateRenderPass failed");
        return false;
    }

    VkPipelineLayoutCreateInfo plci = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr, 0, 1, &dsLayout, 0, nullptr
    };
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout_) != VK_SUCCESS) {
        MLOG_ERR("vkCreatePipelineLayout failed");
        return false;
    }

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = composite_vert_spv_len;
    smci.pCode    = (const uint32_t*)composite_vert_spv;
    VkShaderModule cVert;
    vkCreateShaderModule(dev, &smci, nullptr, &cVert);
    smci.codeSize = composite_frag_spv_len;
    smci.pCode    = (const uint32_t*)composite_frag_spv;
    VkShaderModule cFrag;
    vkCreateShaderModule(dev, &smci, nullptr, &cFrag);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = cVert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = cFrag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vis = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo ias = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE
    };
    VkPipelineViewportStateCreateInfo vps = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr, 0, 1, nullptr, 1, nullptr  // dynamic
    };
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        nullptr, 0, 2, dynStates
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr, 0, VK_FALSE, VK_FALSE,
        VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE,
        0, 0, 0, 0, 1.0f
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr, 0, VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba
    };

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2; gpci.pStages = stages;
    gpci.pVertexInputState   = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState      = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pColorBlendState    = &cbs;
    gpci.pDynamicState       = &dsi;
    gpci.layout      = pipeLayout_;
    gpci.renderPass  = rp_;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe_);
    vkDestroyShaderModule(dev, cFrag, nullptr);
    vkDestroyShaderModule(dev, cVert, nullptr);
    if (r != VK_SUCCESS) {
        MLOG_ERR("vkCreateGraphicsPipelines failed (%d)", r);
        return false;
    }
    return true;
}
