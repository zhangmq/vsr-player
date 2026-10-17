#include "VulkanContext.h"
#include "Log.h"
#include <libplacebo/vulkan.h>
#include <libplacebo/log.h>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char*);
VkResult vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VkResult vkEnumerateInstanceExtensionProperties(const char*, uint32_t*, VkExtensionProperties*);
VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
void vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
}

/// loader 是否提供某个 instance 扩展（决定 HDR 色彩空间能否枚举）。
static bool instance_extension_available(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) != VK_SUCCESS)
        return false;
    for (const auto &e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VulkanContext::~VulkanContext() {
    if (plVk_)  pl_vulkan_destroy(&plVk_);
    if (plLog_) pl_log_destroy(&plLog_);
    if (inst_)  vkDestroyInstance(inst_, nullptr);
}

bool VulkanContext::init() {
    // HDR 上屏前提：VK_EXT_swapchain_colorspace 是 **instance** 扩展——未启用时
    // vkGetPhysicalDeviceSurfaceFormatsKHR 只返回 SRGB 色彩空间，
    // 于是 QRhiSwapChain::isFormatSupported(HDR10/scRGB/P3) 恒 false
    //（spike 实测：启用后同样一块 surface 列出 28 组 format/colorspace，
    //  HDR-capable 若干；见 docs/hdr-pipeline-investigation.md §10.1/§11.1）。
    // 防御：loader 不支持时**不能**让 vkCreateInstance 失败（老驱动/loader 上
    // 只是没有 HDR 能力），故先枚举可用性再决定是否加入。
    static const char *kSwapchainColorSpaceExt = "VK_EXT_swapchain_colorspace";
    std::vector<const char *> instExts = { VK_KHR_SURFACE_EXTENSION_NAME,
                                           "VK_KHR_wayland_surface" };
    if (instance_extension_available(kSwapchainColorSpaceExt)) {
        instExts.push_back(kSwapchainColorSpaceExt);
        swapchainColorspaceExt_ = true;
        MLOG_INFO("instance ext %s enabled (HDR swapchain 色彩空间可枚举)",
                  kSwapchainColorSpaceExt);
    } else {
        MLOG_INFO("instance ext %s NOT available → HDR 上屏不可用（保持 SDR）",
                  kSwapchainColorSpaceExt);
    }

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                             "vsr-player", 0, "vsr", 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
                                 &ai, 0, nullptr,
                                 (uint32_t)instExts.size(), instExts.data() };
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
