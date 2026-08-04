#pragma once
#include <vulkan/vulkan.h>

/// Owns the composite pipeline: render pass + pipeline layout + graphics
/// pipeline that draws the mpv render target (sampled via descriptor set)
/// as a fullscreen triangle. Render pass only describes compatibility
/// (actual rendering happens inside Qt's render pass).
class CompositePipeline {
public:
    ~CompositePipeline();

    /// Create render pass (B8G8R8A8, load/store) + pipeline with the
    /// given descriptor set layout (owned by Video). Dynamic viewport/scissor.
    /// Returns false with message printed to stderr on failure.
    bool create(VkDevice dev, VkDescriptorSetLayout dsLayout);

    VkPipeline pipeline() const { return pipe_; }
    VkPipelineLayout pipelineLayout() const { return pipeLayout_; }

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    VkRenderPass rp_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
};
