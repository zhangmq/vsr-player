/// CUDA-Vulkan interop texture: shared device memory via POSIX fd export/import.
///
/// Flow:
///   1. Create VkImage with VkExternalMemoryImageCreateInfo (OPAQUE_FD)
///   2. Allocate memory with VkExportMemoryAllocateInfo (OPAQUE_FD)
///   3. Query row pitch via vkGetImageSubresourceLayout
///   4. Export fd via vkGetMemoryFdKHR
///   5. Import fd into CUDA via cuImportExternalMemory
///   6. Map CUDA buffer via cuExternalMemoryGetMappedBuffer
///
/// After init(), the texture is writable from CUDA (cuMemcpy2D to cudaPtr())
/// and readable from Vulkan (sample via imageView()). No PCIe round-trip.

#include "InteropTexture.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

namespace vsr {

// ── Helper ──────────────────────────────────────────────────────────

/// Find the first memory type that satisfies both the image's requirements
/// (via type_bits) and the caller's property flags.
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

// ── Lifecycle ──────────────────────────────────────────────────────

InteropTexture::InteropTexture() = default;

InteropTexture::~InteropTexture() { release(); }

// ── Init ───────────────────────────────────────────────────────────

bool InteropTexture::init(VkDevice_T* dev, VkPhysicalDevice_T* pd,
                           uint32_t w, uint32_t h, uint32_t format) {
    VkDevice device = (VkDevice)dev;
    VkPhysicalDevice physDev = (VkPhysicalDevice)pd;
    VkResult res;

    device_ = dev;
    w_ = w;
    h_ = h;
    format_ = format;

    // ---- 1. Create image with external memory support ----
    VkExternalMemoryImageCreateInfo extInfo = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extInfo;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    VkImage img;
    res = vkCreateImage(device, &ici, nullptr, &img);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkCreateImage failed (%d)\n", res);
        release();
        return false;
    }
    image_ = (VkImage_T*)img;

    // ---- 2. Memory requirements ----
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, img, &memReq);

    // ---- 3. Choose memory type ----
    // Prefer DEVICE_LOCAL for GPU-only access via CUDA.
    // Fall back to HOST_VISIBLE|HOST_COHERENT if needed (some implementations
    // only allow linear images in host-visible memory).
    uint32_t memType = find_memory_type(physDev, memReq.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == ~0u) {
        memType = find_memory_type(physDev, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (memType == ~0u) {
        fprintf(stderr, "InteropTexture: no suitable memory type\n");
        release();
        return false;
    }

    // ---- 4. Allocate exportable memory ----
    VkExportMemoryAllocateInfo exportInfo = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exportInfo;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memType;

    VkDeviceMemory mem;
    res = vkAllocateMemory(device, &mai, nullptr, &mem);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkAllocateMemory failed (%d)\n", res);
        release();
        return false;
    }
    memory_ = (VkDeviceMemory_T*)mem;

    // ---- 5. Bind image to memory ----
    res = vkBindImageMemory(device, img, mem, 0);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkBindImageMemory failed (%d)\n", res);
        release();
        return false;
    }

    // ---- 6. Query row pitch ----
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, img, &sub, &layout);
    cudaPitch_ = layout.rowPitch;

    // ---- 7. Export fd ----
    // vkGetMemoryFdKHR is a device extension function — load via proc addr
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    if (!vkGetMemoryFdKHR) {
        fprintf(stderr, "InteropTexture: vkGetMemoryFdKHR not found\n");
        release();
        return false;
    }

    VkMemoryGetFdInfoKHR fdInfo = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fdInfo.memory = mem;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    res = vkGetMemoryFdKHR(device, &fdInfo, &fd);
    if (res != VK_SUCCESS || fd < 0) {
        fprintf(stderr, "InteropTexture: vkGetMemoryFdKHR failed (%d)\n", res);
        release();
        return false;
    }

    // ---- 8. Import into CUDA ----
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extDesc = {};
    extDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    extDesc.handle.fd = fd;
    extDesc.size = memReq.size;
    extDesc.flags = 0;

    CUexternalMemory extMem = nullptr;
    CUresult cuRes = cuImportExternalMemory(&extMem, &extDesc);
    close(fd);  // CUDA driver duplicated the fd internally
    if (cuRes != CUDA_SUCCESS) {
        fprintf(stderr, "InteropTexture: cuImportExternalMemory failed (%d)\n",
                cuRes);
        release();
        return false;
    }
    extMem_ = (void*)extMem;

    // ---- 9. Map CUDA buffer ----
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc = {};
    bufDesc.offset = 0;
    bufDesc.size = memReq.size;
    bufDesc.flags = 0;

    CUdeviceptr cudaPtr = 0;
    cuRes = cuExternalMemoryGetMappedBuffer(&cudaPtr, extMem, &bufDesc);
    if (cuRes != CUDA_SUCCESS) {
        fprintf(stderr,
                "InteropTexture: cuExternalMemoryGetMappedBuffer failed (%d)\n",
                cuRes);
        release();
        return false;
    }
    cudaPtr_ = cudaPtr;

    // ---- 10. Create image view (for Vulkan sampler binding) ----
    VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = (VkFormat)format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;

    VkImageView view;
    res = vkCreateImageView(device, &ivci, nullptr, &view);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkCreateImageView failed (%d)\n", res);
        release();
        return false;
    }
    imageView_ = (VkImageView_T*)view;

    fprintf(stderr, "InteropTexture: %ux%u pitch=%zu cudaPtr=0x%lx\n",
            w, h, cudaPitch_, (unsigned long)cudaPtr);
    return true;
}

// ── Release ────────────────────────────────────────────────────────

void InteropTexture::release() {
    VkDevice device = (VkDevice)device_;

    // Vulkan resources (require VkDevice)
    if (device) {
        vkDeviceWaitIdle(device);

        if (imageView_) {
            vkDestroyImageView(device, (VkImageView)imageView_, nullptr);
            imageView_ = nullptr;
        }
        if (memory_) {
            vkFreeMemory(device, (VkDeviceMemory)memory_, nullptr);
            memory_ = nullptr;
        }
        if (image_) {
            vkDestroyImage(device, (VkImage)image_, nullptr);
            image_ = nullptr;
        }
    }

    // CUDA external memory — destroys the imported handle and unmaps
    // any buffers mapped from it (cudaPtr_ is invalidated automatically).
    if (extMem_) {
        cuDestroyExternalMemory((CUexternalMemory)extMem_);
        extMem_ = nullptr;
    }

    cudaPtr_ = 0;
    cudaPitch_ = 0;
    w_ = 0;
    h_ = 0;
    format_ = 0;
    device_ = nullptr;
}

}  // namespace vsr
