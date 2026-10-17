#pragma once
#include <vulkan/vulkan.h>

/// Owns the composite pipeline: render pass + pipeline layout + graphics
/// pipeline that draws the mpv render target (sampled via descriptor set)
/// as a fullscreen triangle. Render pass only describes compatibility
/// (actual rendering happens inside Qt's render pass).
class CompositePipeline {
public:
    ~CompositePipeline();

    /// Create render pass + pipeline with the given descriptor set layout
    /// (owned by Video). Dynamic viewport/scissor.
    /// `attachmentFormat` **必须匹配 Qt swapchain 的实际格式**：本 render pass
    /// 只描述兼容性（真实 render pass 由 Qt 开始），格式不一致 → pipeline 与
    /// Qt 的 render pass 不兼容（验证层报错/黑屏）。取值见
    /// hdr::vkFormatForMode()：SDR = B8G8R8A8_UNORM，HDR10 =
    /// A2B10G10R10_UNORM_PACK32（scRGB = R16G16B16A16_SFLOAT）。
    /// Returns false with message printed to stderr on failure.
    bool create(VkDevice dev, VkDescriptorSetLayout dsLayout,
                VkFormat attachmentFormat);

    VkPipeline pipeline() const { return pipe_; }
    VkPipelineLayout pipelineLayout() const { return pipeLayout_; }

private:
    /// 销毁 render pass/pipeline/layout（**调用方保证 GPU 已排空**）。
    void destroy();

    VkDevice dev_ = VK_NULL_HANDLE;
    VkRenderPass rp_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
};
