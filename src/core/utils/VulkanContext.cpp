#include "VulkanContext.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

namespace vsr {

// ── Helper ──────────────────────────────────────────────────────────

uint32_t find_memory_type(void* physical_device, uint32_t type_bits,
                          uint32_t memory_property_flags) {
    VkPhysicalDevice pd = (VkPhysicalDevice)physical_device;
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((type_bits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & memory_property_flags) == memory_property_flags)
            return i;
    return ~0u;
}

// ── Lifecycle ───────────────────────────────────────────────────────

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() { release(); }

// ── Init ────────────────────────────────────────────────────────────

bool VulkanContext::init(void* native_window, void* native_display) {
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
        release();
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
        release();
        return false;
    }

    // ---- Physical device ----
    uint32_t count;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "Vulkan: no physical devices found\n");
        release();
        return false;
    }
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
    if (queue_family_ < 0) {
        fprintf(stderr, "Vulkan: no graphics+present queue\n");
        release();
        return false;
    }

    // ---- Device ----
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = (uint32_t)queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    const char* dev_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3;
    dci.ppEnabledExtensionNames = dev_exts;

    VkDevice dev;
    res = vkCreateDevice(pd, &dci, nullptr, &dev);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateDevice failed (%d)\n", res);
        release();
        return false;
    }
    device_ = dev;
    vkGetDeviceQueue(dev, queue_family_, 0, (VkQueue*)&queue_);

    // ---- Command pool + buffer ----
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = queue_family_;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cp;
    res = vkCreateCommandPool(dev, &cpci, nullptr, &cp);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateCommandPool failed (%d)\n", res);
        release();
        return false;
    }
    command_pool_ = cp;

    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cp;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    res = vkAllocateCommandBuffers(dev, &cbai, &cb);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkAllocateCommandBuffers failed (%d)\n", res);
        release();
        return false;
    }
    command_buffer_ = cb;

    // ---- Sync ----
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    res = vkCreateSemaphore(dev, &si, nullptr, (VkSemaphore*)&image_available_semaphore_);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateSemaphore (image_available) failed (%d)\n", res);
        release();
        return false;
    }
    res = vkCreateSemaphore(dev, &si, nullptr, (VkSemaphore*)&render_finished_semaphore_);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateSemaphore (render_finished) failed (%d)\n", res);
        release();
        return false;
    }

    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    res = vkCreateFence(dev, &fci, nullptr, (VkFence*)&fence_);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateFence failed (%d)\n", res);
        release();
        return false;
    }

    // ---- Sampler ----
    VkSamplerCreateInfo sci2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci2.magFilter = VK_FILTER_LINEAR;
    sci2.minFilter = VK_FILTER_LINEAR;
    sci2.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sp2;
    res = vkCreateSampler(dev, &sci2, nullptr, &sp2);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: vkCreateSampler failed (%d)\n", res);
        release();
        return false;
    }
    sampler_ = sp2;

    printf("Vulkan: initialized\n");
    return true;
}

// ── Release ─────────────────────────────────────────────────────────

void VulkanContext::release() {
    // Always destroy surface and instance even if device creation
    // failed partway through init(). Handle device-specific resources
    // only when the device was successfully created.
    VkInstance inst = (VkInstance)instance_;
    VkSurfaceKHR surf = (VkSurfaceKHR)surface_;
    VkDevice dev = (VkDevice)device_;

    if (dev) {
        vkDeviceWaitIdle(dev);

        if (sampler_) { vkDestroySampler(dev, (VkSampler)sampler_, nullptr); sampler_ = nullptr; }
        if (fence_) { vkDestroyFence(dev, (VkFence)fence_, nullptr); fence_ = nullptr; }
        if (image_available_semaphore_) { vkDestroySemaphore(dev, (VkSemaphore)image_available_semaphore_, nullptr); image_available_semaphore_ = nullptr; }
        if (render_finished_semaphore_) { vkDestroySemaphore(dev, (VkSemaphore)render_finished_semaphore_, nullptr); render_finished_semaphore_ = nullptr; }
        if (command_buffer_) { /* freed with command pool */ command_buffer_ = nullptr; }
        if (command_pool_) { vkDestroyCommandPool(dev, (VkCommandPool)command_pool_, nullptr); command_pool_ = nullptr; }
        if (queue_) { queue_ = nullptr; }

        vkDestroyDevice(dev, nullptr);
        device_ = nullptr;
    }

    if (surf) {
        vkDestroySurfaceKHR(inst, surf, nullptr);
        surface_ = nullptr;
    }
    if (inst) {
        vkDestroyInstance(inst, nullptr);
        instance_ = nullptr;
    }
}

}  // namespace vsr
