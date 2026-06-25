#pragma once

#include "api/Player.h"  // IVulkanContext

#include <vulkan/vulkan.h>

class QQuickView;

namespace vsr {

/// Qt Quick implementation of IVulkanContext.
/// Extracts Vulkan handles from Qt's QSGRendererInterface at call time.
/// The QQuickView must outlive this object.
class QtVulkanContext : public IVulkanContext {
public:
    explicit QtVulkanContext(QQuickView* view);

    void* vkInstance()       const override;
    void* vkPhysicalDevice() const override;
    void* vkDevice()         const override;
    void* vkQueue()          const override;
    int   vkQueueFamily()    const override;
    void* vkCommandPool()    const override;
    void* vkRenderPass()     const override;

    void waitIdle() const override;
    void submitAndWait(void* commandBuffer) const override;

    void setQueueFamily(int qf) { queueFamily_ = qf; }
    void setRenderPass(VkRenderPass rp) { renderPass_ = rp; }

    /// Create a transient command pool (called once after Vulkan is ready).
    /// Must be called after setQueueFamily — uses stored queue family index.
    void initCommandPool();

    /// Destroy the command pool (called before VkDevice is destroyed).
    void destroyCommandPool();

private:
    QQuickView* view_;
    int queueFamily_ = 0;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
};

}  // namespace vsr
