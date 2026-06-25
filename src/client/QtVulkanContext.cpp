#include "QtVulkanContext.h"

#include <QQuickView>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>

#include <cstdio>

namespace vsr {

QtVulkanContext::QtVulkanContext(QQuickView* view) : view_(view) {}

void* QtVulkanContext::vkInstance() const {
    auto* rif = view_->rendererInterface();
    if (!rif) return nullptr;
    void* r = rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource);
    if (!r) return nullptr;
    return static_cast<QVulkanInstance*>(r)->vkInstance();
}

void* QtVulkanContext::vkPhysicalDevice() const {
    auto* rif = view_->rendererInterface();
    if (!rif) return nullptr;
    void* r = rif->getResource(view_, QSGRendererInterface::PhysicalDeviceResource);
    return r ? *static_cast<VkPhysicalDevice*>(r) : nullptr;
}

void* QtVulkanContext::vkDevice() const {
    auto* rif = view_->rendererInterface();
    if (!rif) return nullptr;
    void* r = rif->getResource(view_, QSGRendererInterface::DeviceResource);
    return r ? *static_cast<VkDevice*>(r) : nullptr;
}

void* QtVulkanContext::vkQueue() const {
    auto* rif = view_->rendererInterface();
    if (!rif) return nullptr;
    void* r = rif->getResource(view_, QSGRendererInterface::CommandQueueResource);
    return r ? *static_cast<VkQueue*>(r) : nullptr;
}

int QtVulkanContext::vkQueueFamily() const { return queueFamily_; }

void* QtVulkanContext::vkCommandPool() const { return (void*)commandPool_; }

void* QtVulkanContext::vkRenderPass() const { return (void*)renderPass_; }

void QtVulkanContext::waitIdle() const {
    auto* rif = view_->rendererInterface();
    void* r = rif->getResource(view_, QSGRendererInterface::DeviceResource);
    if (r) {
        auto* vi = static_cast<QVulkanInstance*>(
            rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource));
        vi->deviceFunctions(*static_cast<VkDevice*>(r))
            ->vkDeviceWaitIdle(*static_cast<VkDevice*>(r));
    }
}

void QtVulkanContext::submitAndWait(void* cb) const {
    auto* rif = view_->rendererInterface();
    void* r = rif->getResource(view_, QSGRendererInterface::CommandQueueResource);
    if (!r) return;
    VkQueue queue = *static_cast<VkQueue*>(r);

    VkCommandBuffer cmdBuf = (VkCommandBuffer)cb;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuf;

    auto* vi = static_cast<QVulkanInstance*>(
        rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource));
    void* rdev = rif->getResource(view_, QSGRendererInterface::DeviceResource);
    auto* vkdf = vi->deviceFunctions(*static_cast<VkDevice*>(rdev));
    vkdf->vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkdf->vkQueueWaitIdle(queue);
}

void QtVulkanContext::initCommandPool() {
    VkDevice dev = (VkDevice)vkDevice();
    if (!dev || commandPool_) return;

    auto* rif = view_->rendererInterface();
    if (!rif) return;
    auto* vi = static_cast<QVulkanInstance*>(
        rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource));
    auto* vkdf = vi->deviceFunctions(dev);

    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = (uint32_t)queueFamily_;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool;
    if (vkdf->vkCreateCommandPool(dev, &ci, nullptr, &pool) == VK_SUCCESS) {
        commandPool_ = pool;
        fprintf(stderr, "QtVulkanContext: created transient command pool (qf=%d)\n",
                queueFamily_);
    }
}

void QtVulkanContext::destroyCommandPool() {
    if (commandPool_ == VK_NULL_HANDLE) return;
    VkDevice dev = (VkDevice)vkDevice();
    if (!dev) return;

    auto* rif = view_->rendererInterface();
    if (!rif) return;
    auto* vi = static_cast<QVulkanInstance*>(
        rif->getResource(view_, QSGRendererInterface::VulkanInstanceResource));
    auto* vkdf = vi->deviceFunctions(dev);

    vkdf->vkDestroyCommandPool(dev, commandPool_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
}

}  // namespace vsr
