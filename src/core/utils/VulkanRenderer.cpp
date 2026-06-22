/// Minimal Vulkan renderer for video frames.

#include "VulkanRenderer.h"

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

// ── Helper ──────────────────────────────────────────────────────────

static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_bits,
                                 VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((type_bits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return ~0u;
}

// ── Lifecycle ───────────────────────────────────────────────────────

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { release(); }

// ── Init ────────────────────────────────────────────────────────────

bool VulkanRenderer::init(void* native_window, void* native_display) {
    VkResult res;

    // ---- Instance ----
    uint32_t avail_ver = 0;
    vkEnumerateInstanceVersion(&avail_ver);
    fprintf(stderr, "Vulkan: loader instance version %d.%d.%d\n",
            VK_API_VERSION_MAJOR(avail_ver),
            VK_API_VERSION_MINOR(avail_ver),
            VK_API_VERSION_PATCH(avail_ver));

    uint32_t api_ver = VK_API_VERSION_1_3;
    if (avail_ver > 0 && api_ver > avail_ver)
        api_ver = avail_ver;

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vsr-player";
    ai.apiVersion = api_ver;

    const char* exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;

    VkInstance inst;
    res = vkCreateInstance(&ici, nullptr, &inst);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateInstance failed (error %d)\n", res);
        return false;
    }
    instance_ = inst;

    // ---- Wayland Surface ----
    if (!native_display || !native_window) {
        fprintf(stderr, "Vulkan: native_display and native_window required\n");
        return false;
    }

    VkWaylandSurfaceCreateInfoKHR sci = {
        VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    sci.display = (wl_display*)native_display;
    sci.surface = (wl_surface*)native_window;
    res = vkCreateWaylandSurfaceKHR(inst, &sci, nullptr,
                                    (VkSurfaceKHR*)&surface_);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: Wayland surface creation failed (%d)\n", res);
        return false;
    }

    // ---- Physical device ----
    uint32_t count;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> pds(count);
    vkEnumeratePhysicalDevices(inst, &count, pds.data());
    VkPhysicalDevice pd = pds[0];
    physical_device_ = pd;

    // Find graphics + present queue
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, qfps.data());
    queue_family_ = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 ok;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, (VkSurfaceKHR)surface_, &ok);
        if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && ok) {
            queue_family_ = (int)i; break;
        }
    }
    if (queue_family_ < 0) { fprintf(stderr, "Vulkan: no graphics+present queue\n"); return false; }

    // ---- Device ----
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = (uint32_t)queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    VkDevice dev;
    vkCreateDevice(pd, &dci, nullptr, &dev);
    device_ = dev;
    vkGetDeviceQueue(dev, queue_family_, 0, (VkQueue*)&queue_);

    // ---- Command pool + buffer ----
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = queue_family_;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cp;
    vkCreateCommandPool(dev, &cpci, nullptr, &cp);
    command_pool_ = cp;

    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cp;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cbai, &cb);
    command_buffer_ = cb;

    // ---- Sync ----
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &si, nullptr, (VkSemaphore*)&image_available_semaphore_);
    vkCreateSemaphore(dev, &si, nullptr, (VkSemaphore*)&render_finished_semaphore_);

    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev, &fci, nullptr, (VkFence*)&fence_);

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

    printf("Vulkan: initialized\n");
    return true;
}

// ── Swapchain + pipeline ────────────────────────────────────────────

bool VulkanRenderer::create_swapchain_and_pipeline(int w, int h) {
    VkDevice dev = (VkDevice)device_;
    VkPhysicalDevice pd = (VkPhysicalDevice)physical_device_;
    if (!dev || w <= 0 || h <= 0) return false;

    // Destroy old
    for (auto* fb : framebuffers_)
        vkDestroyFramebuffer(dev, (VkFramebuffer)fb, nullptr);
    for (auto* iv : swapchain_image_views_)
        vkDestroyImageView(dev, (VkImageView)iv, nullptr);
    if (pipeline_) vkDestroyPipeline(dev, (VkPipeline)pipeline_, nullptr);
    if (render_pass_) vkDestroyRenderPass(dev, (VkRenderPass)render_pass_, nullptr);
    if (sampler_) vkDestroySampler(dev, (VkSampler)sampler_, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(dev, (VkSwapchainKHR)swapchain_, nullptr);
    framebuffers_.clear();
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, (VkSurfaceKHR)surface_, &caps);
    w = std::max(w, (int)caps.minImageExtent.width);
    w = std::min(w, (int)caps.maxImageExtent.width);
    h = std::max(h, (int)caps.minImageExtent.height);
    h = std::min(h, (int)caps.maxImageExtent.height);
    swapchain_width_ = w;
    swapchain_height_ = h;

    // Swapchain
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = (VkSurfaceKHR)surface_;
    sci.minImageCount = 2;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {(uint32_t)w, (uint32_t)h};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    VkSwapchainKHR sc;
    vkCreateSwapchainKHR(dev, &sci, nullptr, &sc);
    swapchain_ = sc;

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(dev, sc, &ic, nullptr);
    std::vector<VkImage> sc_images(ic);
    vkGetSwapchainImagesKHR(dev, sc, &ic, sc_images.data());

    // Render pass
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
    vkCreateRenderPass(dev, &rpci, nullptr, &rp);
    render_pass_ = rp;

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

    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D scissor = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
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
    pci.layout = pl; pci.renderPass = rp; pci.subpass = 0;

    VkPipeline pipe;
    vkCreateGraphicsPipelines(dev, nullptr, 1, &pci, nullptr, &pipe);
    pipeline_ = pipe;

    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);

    // Framebuffers + image views
    swapchain_images_.resize(ic);
    swapchain_image_views_.resize(ic);
    framebuffers_.resize(ic);
    for (uint32_t i = 0; i < ic; i++) {
        VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = sc_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = sci.imageFormat;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;

        VkImageView iv;
        vkCreateImageView(dev, &ivci, nullptr, &iv);
        swapchain_image_views_[i] = iv;

        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.attachmentCount = 1;
        fci.pAttachments = (VkImageView*)&swapchain_image_views_[i];
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;

        VkFramebuffer fb;
        vkCreateFramebuffer(dev, &fci, nullptr, &fb);
        framebuffers_[i] = fb;
    }

    // Sampler
    VkSamplerCreateInfo sci2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci2.magFilter = VK_FILTER_LINEAR;
    sci2.minFilter = VK_FILTER_LINEAR;
    sci2.addressModeU = sci2.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sp2;
    vkCreateSampler(dev, &sci2, nullptr, &sp2);
    sampler_ = sp2;

    printf("Vulkan: swapchain %dx%d, %u images\n", w, h, ic);
    return true;
}

// ── Texture update ──────────────────────────────────────────────────

bool VulkanRenderer::update_texture(const uint8_t* data, int w, int h, int bpp) {
    VkDevice dev = (VkDevice)device_;
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
        mai.memoryTypeIndex = find_memory_type((VkPhysicalDevice)physical_device_,
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
        fprintf(stderr, "Vulkan: texture %dx%d rowPitch=%zu (w*4=%d)\n",
                w, h, tex_row_pitch_, w * 4);
        VkFence f = (VkFence)fence_;
        vkWaitForFences(dev, 1, (VkFence*)&f, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, (VkFence*)&f);
        VkCommandBuffer cb = (VkCommandBuffer)command_buffer_;
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
        vkQueueSubmit((VkQueue)queue_, 1, &si, f);
        vkWaitForFences(dev, 1, (VkFence*)&f, VK_TRUE, UINT64_MAX);
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
    dii.sampler = (VkSampler)sampler_;
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
    VkDevice dev = (VkDevice)device_;
    if (!dev || !surface_ || !swapchain_) return false;

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
    uint32_t idx;
    VkResult res = vkAcquireNextImageKHR(dev, (VkSwapchainKHR)swapchain_,
        UINT64_MAX, (VkSemaphore)image_available_semaphore_, nullptr, &idx);
    if (res != VK_SUCCESS) return false;

    // Record + submit
    VkFence f = (VkFence)fence_;
    VkCommandBuffer cb = (VkCommandBuffer)command_buffer_;
    vkWaitForFences(dev, 1, (VkFence*)&f, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, (VkFence*)&f);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue cv = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = (VkRenderPass)render_pass_;
    rpbi.framebuffer = (VkFramebuffer)framebuffers_[idx];
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
    si.pWaitSemaphores = (VkSemaphore*)&image_available_semaphore_;
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = (VkSemaphore*)&render_finished_semaphore_;
    vkQueueSubmit((VkQueue)queue_, 1, &si, f);

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = (VkSemaphore*)&render_finished_semaphore_;
    pi.swapchainCount = 1;
    pi.pSwapchains = (VkSwapchainKHR*)&swapchain_;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR((VkQueue)queue_, &pi);

    return true;
}

// ── Release ─────────────────────────────────────────────────────────

void VulkanRenderer::release() {
    VkDevice dev = (VkDevice)device_;
    if (!dev) return;
    vkDeviceWaitIdle(dev);

    for (auto* fb : framebuffers_) vkDestroyFramebuffer(dev, (VkFramebuffer)fb, nullptr);
    for (auto* iv : swapchain_image_views_) vkDestroyImageView(dev, (VkImageView)iv, nullptr);
    framebuffers_.clear();
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    if (pipeline_) { vkDestroyPipeline(dev, (VkPipeline)pipeline_, nullptr); pipeline_ = nullptr; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(dev, (VkPipelineLayout)pipeline_layout_, nullptr); pipeline_layout_ = nullptr; }
    if (render_pass_) { vkDestroyRenderPass(dev, (VkRenderPass)render_pass_, nullptr); render_pass_ = nullptr; }
    if (swapchain_) { vkDestroySwapchainKHR(dev, (VkSwapchainKHR)swapchain_, nullptr); swapchain_ = nullptr; }
    if (sampler_) { vkDestroySampler(dev, (VkSampler)sampler_, nullptr); sampler_ = nullptr; }
    if (descriptor_pool_) { vkDestroyDescriptorPool(dev, (VkDescriptorPool)descriptor_pool_, nullptr); descriptor_pool_ = nullptr; }
    if (descriptor_set_layout_) { vkDestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout)descriptor_set_layout_, nullptr); descriptor_set_layout_ = nullptr; }
    if (fence_) { vkDestroyFence(dev, (VkFence)fence_, nullptr); fence_ = nullptr; }
    if (image_available_semaphore_) { vkDestroySemaphore(dev, (VkSemaphore)image_available_semaphore_, nullptr); image_available_semaphore_ = nullptr; }
    if (render_finished_semaphore_) { vkDestroySemaphore(dev, (VkSemaphore)render_finished_semaphore_, nullptr); render_finished_semaphore_ = nullptr; }
    if (command_pool_) { vkDestroyCommandPool(dev, (VkCommandPool)command_pool_, nullptr); command_pool_ = nullptr; }
    if (texture_) { vkDestroyImage(dev, (VkImage)texture_, nullptr); texture_ = nullptr; }
    if (texture_memory_) { vkFreeMemory(dev, (VkDeviceMemory)texture_memory_, nullptr); texture_memory_ = nullptr; }

    if (surface_) { vkDestroySurfaceKHR((VkInstance)instance_, (VkSurfaceKHR)surface_, nullptr); surface_ = nullptr; }
    if (device_) { vkDestroyDevice(dev, nullptr); device_ = nullptr; }
    if (instance_) { vkDestroyInstance((VkInstance)instance_, nullptr); instance_ = nullptr; }

    printf("Vulkan: released\n");
}

}  // namespace vsr
