#pragma once

#include <cstddef>
#include <cstdint>
#include <cuda.h>

// Opaque forward declarations of Vulkan handle structs.
// These match the actual struct tags behind VK_DEFINE_HANDLE / VK_DEFINE_NON_DISPATCHABLE_HANDLE
// in vulkan.h (e.g. VkDevice is typedef struct VkDevice_T* VkDevice).
// We use pointer-to-struct syntax directly so that both this header and vulkan.h can coexist.
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkImage_T;
struct VkImageView_T;
struct VkDeviceMemory_T;

namespace vsr {

/// CUDA-Vulkan interop texture using external memory (POSIX fd).
///
/// Creates a Vulkan image backed by exportable device memory, imports the
/// same physical memory into CUDA, and exposes a CUdeviceptr for direct GPU
/// writes. Both APIs access the same physical GPU memory — zero-copy interop.
///
/// The image is created with VK_IMAGE_TILING_LINEAR and left in
/// VK_IMAGE_LAYOUT_PREINITIALIZED. The caller is responsible for transitioning
/// the layout (e.g., to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) before
/// rendering.
class InteropTexture {
public:
    InteropTexture();
    ~InteropTexture();

    /// Create the shared texture.
    /// A CUDA context must be current on the calling thread.
    /// @param dev     VkDevice (must have VK_KHR_external_memory_fd enabled)
    /// @param pd      VkPhysicalDevice
    /// @param w       Width in pixels
    /// @param h       Height in pixels
    /// @param format  VkFormat (e.g. VK_FORMAT_R8G8B8A8_UNORM = 37)
    bool init(VkDevice_T* dev, VkPhysicalDevice_T* pd,
              uint32_t w, uint32_t h, uint32_t format);

    /// Release all Vulkan and CUDA resources.
    void release();

    // ── Accessors ──────────────────────────────────────────────────
    VkImageView_T* imageView() const { return imageView_; }
    CUdeviceptr cudaPtr() const { return cudaPtr_; }
    size_t cudaPitch() const { return cudaPitch_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    VkImage_T* vulkanImage() const { return image_; }
    bool valid() const { return image_ != nullptr; }

    /// Record a pipeline barrier to transition this image's layout.
    /// cb must be in recording state. Caller submits + waits.
    void transitionLayout(void* device, void* commandBuffer,
                          uint32_t oldLayout, uint32_t newLayout);

private:
    VkDevice_T*       device_ = nullptr;
    VkImage_T*        image_ = nullptr;
    VkDeviceMemory_T* memory_ = nullptr;
    VkImageView_T*    imageView_ = nullptr;
    CUdeviceptr       cudaPtr_ = 0;
    size_t            cudaPitch_ = 0;
    uint32_t          w_ = 0, h_ = 0;
    uint32_t          format_ = 0;
    void*             extMem_ = nullptr;  // CUexternalMemory
};

}  // namespace vsr
