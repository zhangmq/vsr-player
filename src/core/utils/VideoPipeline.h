#pragma once

#include <cstdint>
#include <vector>

#include "InteropTexture.h"

namespace vsr {

struct TextureBinding {
    uint32_t binding;     // descriptor binding number (0, 1, ...)
    uint32_t format;      // VkFormat (e.g. VK_FORMAT_R8G8B8A8_UNORM = 37)
    uint32_t width;       // texture width in pixels
    uint32_t height;      // texture height in pixels
};

struct PipelineConfig {
    std::vector<TextureBinding> textures;
    const uint32_t* vertSpv = nullptr;
    size_t vertSpvLen = 0;
    const uint32_t* fragSpv = nullptr;
    size_t fragSpvLen = 0;
};

/// Config-driven Vulkan graphics pipeline for rendering a fullscreen textured quad.
///
/// Owns descriptor set layout + pool + set, pipeline layout, graphics pipeline,
/// a sampler, fragment shader module, and 1 or 2 InteropTextures (depending on
/// the PipelineConfig). The vertex shader is provided by the caller (typically
/// a fullscreen-triangle shader with no vertex buffer).
///
/// Two instances are used in VulkanRenderer:
///   - rgbaPipeline: 1x RGBA8 texture + pass-through shader (VSR path)
///   - nv12Pipeline: R8 + R8G8 textures + NV12-to-RGB shader (NO-VSR path)
///
/// Both share the same vertex shader and the same render pass.
class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    /// Initialize pipeline and interop textures.
    /// @param dev           VkDevice
    /// @param pd            VkPhysicalDevice (for InteropTexture)
    /// @param renderPass    VkRenderPass (shared with SwapchainManager)
    /// @param queue         VkQueue (for layout transition)
    /// @param commandPool   VkCommandPool (for layout transition)
    /// @param cfg           Pipeline configuration (textures, shaders)
    bool init(void* device, void* physicalDevice, void* renderPass,
              void* queue, void* commandPool,
              const PipelineConfig& cfg);

    /// Bind descriptor set and pipeline on the command buffer, draw fullscreen triangle.
    /// Viewport and scissor must already be set by the caller (VulkanRenderer handles
    /// letterboxing).
    void bind(void* commandBuffer);

    /// Access the interop texture at a given index (0 = first texture).
    /// Caller uses these for D2D/H2D copies before render.
    InteropTexture& interopTexture(uint32_t idx = 0);

    /// Release all Vulkan resources. Must be called before VkDevice is destroyed.
    /// InteropTextures are destroyed here (while device is alive), not in the destructor.
    void release(void* device);

    bool ready() const { return pipeline_ != nullptr; }
    void* pipelineLayout() const { return pipeline_layout_; }

private:
    void* descriptor_set_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* descriptor_set_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* pipeline_ = nullptr;
    void* vert_module_ = nullptr;
    void* frag_module_ = nullptr;
    void* sampler_ = nullptr;

    std::vector<InteropTexture> interopTextures_;
};

}  // namespace vsr
