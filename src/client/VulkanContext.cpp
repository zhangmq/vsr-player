#include "VulkanContext.h"
#include "Log.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/log.h>
#include <cstdio>
#include <vector>

extern "C" {
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char*);
VkResult vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
void vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
}

VulkanContext::~VulkanContext() {
    if (plVk_)  pl_vulkan_destroy(&plVk_);
    if (plLog_) pl_log_destroy(&plLog_);
    if (inst_)  vkDestroyInstance(inst_, nullptr);
}

bool VulkanContext::init() {
    const char* instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_wayland_surface" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                             "vsr-player", 0, "vsr", 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
                                 &ai, 0, nullptr, 2, instExts };
    if (vkCreateInstance(&ici, nullptr, &inst_) != VK_SUCCESS) {
        MLOG_ERR("vkCreateInstance failed");
        return false;
    }

    uint32_t nc = 0; vkEnumeratePhysicalDevices(inst_, &nc, nullptr);
    if (nc == 0) { MLOG_ERR("no physical devices"); return false; }
    std::vector<VkPhysicalDevice> pds(nc);
    vkEnumeratePhysicalDevices(inst_, &nc, pds.data());
    pd_ = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd_, &props);
    devName_ = props.deviceName;

    vk12_.timelineSemaphore = VK_TRUE;
    vkGetPhysicalDeviceFeatures2(pd_, &f2_);

    pl_log_params lc = {}; lc.log_level = PL_LOG_WARN;
    plLog_ = pl_log_create(PL_API_VER, &lc);

    // CUDA↔Vulkan interop (nvdec 帧导入 + VSR CUDA 输出导出) 需要 FD 导出/
    // 导入支持。mpv 的 cuda_vk mapper 通过 PL_HANDLE_FD 导入纹理，
    // 设备必须启用这两个扩展，否则 libplacebo 中 vkImportMemoryFdKHR 等
    // 函数指针为 NULL → SIGSEGV。
    devExts_ = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                 VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                 VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    pl_vulkan_params vp = {};
    vp.instance = inst_; vp.get_proc_addr = vkGetInstanceProcAddr;
    vp.device = pd_; vp.features = &f2_;
    vp.extensions = devExts_.data(); vp.num_extensions = (int)devExts_.size();
    vp.queue_count = 1;
    plVk_ = pl_vulkan_create(plLog_, &vp);
    if (!plVk_) { MLOG_ERR("pl_vulkan_create failed"); return false; }
    dev_ = plVk_->device;
    qfi_ = plVk_->queue_graphics.index;
    vkGetDeviceQueue(dev_, qfi_, 0, &queue_);
    return true;
}
