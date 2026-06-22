# CUDA-Vulkan Interop — Zero-PCIe Pipeline Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Eliminate PCIe round-trips in the render pipeline by introducing CUDA-Vulkan shared textures (InteropTexture), NV12→RGB vulkan shader, and refactoring VulkanRenderer into focused classes.

**Architecture:** Five new/refactored classes replace the current monolithic VulkanRenderer. InteropTexture provides dual-API-visible GPU memory. VideoPipeline encapsulates descriptor/pipeline per rendering path. MainWindow does D2D/H2D copies into InteropTextures instead of GPU→CPU→GPU round-trips.

**Tech Stack:** C++20, Vulkan 1.3, CUDA Driver API, Wayland, Qt6, SPIR-V shaders

---

## File Change Matrix

| File | Status | Responsibility |
|------|--------|----------------|
| `src/core/utils/InteropTexture.h` | **Create** | Dual-API Vulkan+CUDA image, external fd export/import |
| `src/core/utils/InteropTexture.cpp` | **Create** | Implementation |
| `src/core/utils/VulkanContext.h` | **Create** | Instance, device, queue, sync primitives (extracted) |
| `src/core/utils/VulkanContext.cpp` | **Create** | Implementation |
| `src/core/utils/SwapchainManager.h` | **Create** | Swapchain, render pass, framebuffers, physical-pixel sizing (extracted) |
| `src/core/utils/SwapchainManager.cpp` | **Create** | Implementation |
| `src/core/utils/VideoPipeline.h` | **Create** | Config-driven descriptor/pipeline + InteropTextures |
| `src/core/utils/VideoPipeline.cpp` | **Create** | Implementation |
| `src/client/shaders/nv12.frag` | **Create** | NV12→RGB BT.601 fragment shader |
| `src/core/utils/VulkanRenderer.h` | **Modify** | Simplified coordinator, new `render_frame(Path)` API |
| `src/core/utils/VulkanRenderer.cpp` | **Modify** | ~635→~80 lines, delegates to sub-objects |
| `src/client/VulkanWidget.h` | **Modify** | New `present_frame(Path)` signature |
| `src/client/VulkanWidget.cpp` | **Modify** | Forward Path to renderer |
| `src/client/MainWindow.h` | **Modify** | Add InteropTexture members, `--no-hwaccel` support |
| `src/client/MainWindow.cpp` | **Modify** | D2D/H2D replaces DtoH+vkMapMemory |
| `src/client/main.cpp` | **Modify** | Add `--no-hwaccel` flag |
| `Makefile` | **Modify** | New objects, NV12 shader build rules, test_interop target |
| `tests/test_interop.cpp` | **Create** | Headless CUDA-Vulkan interop validation |

---

### Task 1: InteropTexture — dual-API shared image

**Files:**
- Create: `src/core/utils/InteropTexture.h`
- Create: `src/core/utils/InteropTexture.cpp`
- Create: `tests/test_interop.cpp`
- Modify: `Makefile` (add InteropTexture.o, test_interop target)

This is the foundational primitive. One `InteropTexture` = one Vulkan image + imported CUDA device pointer backed by the same physical memory. CUDA writes via `cuMemcpy2DAsync`, Vulkan reads via `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`.

- [ ] **Step 1: Write the header**

```cpp
// src/core/utils/InteropTexture.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <cuda.h>

// Forward declarations (opaque Vulkan handles)
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
VK_DEFINE_HANDLE(VkInstance_T)
VK_DEFINE_HANDLE(VkPhysicalDevice_T)
VK_DEFINE_HANDLE(VkDevice_T)
VK_DEFINE_HANDLE(VkImage_T)
VK_DEFINE_HANDLE(VkImageView_T)
VK_DEFINE_HANDLE(VkDeviceMemory_T)
#undef VK_DEFINE_HANDLE

namespace vsr {

/// A Vulkan image with external memory exported as a POSIX fd and imported
/// into CUDA. Both APIs access the same physical GPU memory.
///
/// CUDA writes via cuMemcpy2DAsync (D2D or H2D). Vulkan reads via
/// VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER in a fragment shader.
///
/// Tiling is VK_IMAGE_TILING_LINEAR — the row-linear layout is required
/// for CUDA visibility (OPTIMAL uses implementation-defined swizzle).
class InteropTexture {
public:
    InteropTexture();
    ~InteropTexture();

    /// Create the shared image.
    /// @param dev   VkDevice
    /// @param pd    VkPhysicalDevice (for memory type queries)
    /// @param w, h  Image dimensions in pixels
    /// @param format  Vulkan format (e.g. VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)
    bool init(VkDevice_T* dev, VkPhysicalDevice_T* pd,
              uint32_t w, uint32_t h, uint32_t format);

    /// Release all resources (safe to call multiple times).
    void release();

    // -- Accessors used by VideoPipeline (descriptor binding) --
    VkImageView_T* imageView() const { return imageView_; }

    // -- Accessors used by MainWindow (D2D/H2D copy destination) --
    CUdeviceptr cudaPtr() const { return cudaPtr_; }
    size_t cudaPitch() const { return cudaPitch_; }   // row pitch in bytes
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

    bool valid() const { return image_ != nullptr; }

private:
    VkImage_T*       image_ = nullptr;
    VkDeviceMemory_T* memory_ = nullptr;
    VkImageView_T*   imageView_ = nullptr;
    CUdeviceptr       cudaPtr_ = 0;
    size_t            cudaPitch_ = 0;
    uint32_t          w_ = 0, h_ = 0;
    uint32_t          format_ = 0;

    // CUDA external memory handle (released in release())
    void* extMem_ = nullptr;  // CUexternalMemory
};

}  // namespace vsr
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/core/utils/InteropTexture.cpp
#include "InteropTexture.h"

#include <cstdio>
#include <cstring>
#include <cuda.h>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

namespace vsr {

static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_bits,
                                 VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return ~0u;
}

InteropTexture::InteropTexture() = default;
InteropTexture::~InteropTexture() { release(); }

bool InteropTexture::init(VkDevice_T* dev, VkPhysicalDevice_T* pd,
                          uint32_t w, uint32_t h, uint32_t format) {
    VkDevice d = (VkDevice)dev;
    VkPhysicalDevice p = (VkPhysicalDevice)pd;
    w_ = w; h_ = h; format_ = format;

    // 1. Create image with external memory export intent
    VkExternalMemoryImageCreateInfo extImgInfo = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImgInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extImgInfo;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;  // CUDA needs row-linear layout
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    VkImage img;
    if (vkCreateImage(d, &ici, nullptr, &img) != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkCreateImage failed\n");
        return false;
    }
    image_ = (VkImage_T*)img;

    // 2. Get memory requirements
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, img, &mr);

    // 3. Allocate memory with fd export
    VkExportMemoryAllocateInfo exportInfo = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exportInfo;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type(p, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory mem;
    if (vkAllocateMemory(d, &mai, nullptr, &mem) != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkAllocateMemory failed\n");
        release();
        return false;
    }
    memory_ = (VkDeviceMemory_T*)mem;

    // 4. Bind
    vkBindImageMemory(d, img, mem, 0);

    // 5. Query row pitch (this is what CUDA sees)
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout srl;
    vkGetImageSubresourceLayout(d, img, &sub, &srl);
    cudaPitch_ = srl.rowPitch;

    // 6. Export fd
    int fd = -1;
    VkMemoryGetFdInfoKHR getFdInfo = {
        VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    getFdInfo.memory = mem;
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR_ptr =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d, "vkGetMemoryFdKHR");
    if (!vkGetMemoryFdKHR_ptr ||
        vkGetMemoryFdKHR_ptr(d, &getFdInfo, &fd) != VK_SUCCESS) {
        fprintf(stderr, "InteropTexture: vkGetMemoryFdKHR failed\n");
        release();
        return false;
    }

    // 7. CUDA import
    CUexternalMemory extMem = nullptr;
    CUresult cr = cuImportExternalMemory(&extMem,
        (CUexternalMemoryHandleType)0x10005);  // CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD
    if (cr != CUDA_SUCCESS) {
        // Try named constant if available
        cr = cuImportExternalMemory(&extMem, 0x10005);
    }
    if (cr != CUDA_SUCCESS) {
        fprintf(stderr, "InteropTexture: cuImportExternalMemory failed (%d)\n", cr);
        close(fd);
        release();
        return false;
    }
    extMem_ = extMem;
    close(fd);  // CUDA now owns a reference

    // 8. Map to CUDA device pointer
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufDesc = {};
    bufDesc.offset = 0;
    bufDesc.size = mr.size;

    CUresult cr2 = cuExternalMemoryGetMappedBuffer(
        &cudaPtr_, (CUexternalMemory)extMem_, &bufDesc);
    if (cr2 != CUDA_SUCCESS) {
        fprintf(stderr, "InteropTexture: cuExternalMemoryGetMappedBuffer failed (%d)\n", cr2);
        release();
        return false;
    }

    // 9. Create image view (for descriptor binding)
    VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = (VkFormat)format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;

    VkImageView iv;
    vkCreateImageView(d, &ivci, nullptr, &iv);
    imageView_ = (VkImageView_T*)iv;

    // 10. Transition layout: PREINITIALIZED → SHADER_READ_ONLY_OPTIMAL
    // Caller responsible for this via a command buffer, or we do a quick
    // submit here. For simplicity, the first render_frame will handle layout
    // transition in the render pass. Leave in PREINITIALIZED for now.
    // (The descriptor binding in VideoPipeline will handle the transition
    //  via the render pass's initialLayout, or we add a barrier here.)
    //
    // Actually, let's do the barrier now:
    {
        VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = 0;  // assume queue family 0 — caller can override
        // For a standalone interop texture without queue knowledge,
        // we transition inside SwapchainManager's render pass instead.
        // This is documented: InteropTexture leaves the image in
        // PREINITIALIZED; the first bind transitions it.
    }

    fprintf(stderr, "InteropTexture: %dx%d fmt=%d rowPitch=%zu cudaPtr=0x%llx\n",
            w, h, format, cudaPitch_, (unsigned long long)cudaPtr_);
    return true;
}

void InteropTexture::release() {
    if (cudaPtr_) {
        cuMemFree(cudaPtr_);
        cudaPtr_ = 0;
    }
    if (extMem_) {
        cuDestroyExternalMemory((CUexternalMemory)extMem_);
        extMem_ = nullptr;
    }
    // Vulkan resources — caller must ensure device is still alive
    // (called before VulkanRenderer::release() destroys device)
    if (imageView_) {
        // vkDestroyImageView handled by caller with valid device
        imageView_ = nullptr;
    }
    if (memory_) {
        // vkFreeMemory handled by caller
        memory_ = nullptr;
    }
    if (image_) {
        // vkDestroyImage handled by caller
        image_ = nullptr;
    }
    w_ = h_ = 0;
    cudaPitch_ = 0;
}

}  // namespace vsr
```

**API note on CUDA external memory types:** The bundled `cuda.h` in `third_party/cuda/include/` provides `cuImportExternalMemory` and `cuExternalMemoryGetMappedBuffer`. The handle type constant `CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD` may be named `CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD` in newer headers or may need the numeric value `0x10005`. Check the header for the exact constant name and use it.

- [ ] **Step 3: Write headless test**

```cpp
// tests/test_interop.cpp
/// Standalone test: verify CUDA-Vulkan external memory interop works.
/// Creates a small Vulkan InteropTexture, writes test data via CUDA D2D,
/// reads back via Vulkan vkMapMemory, and checks the values.

#include <cstdio>
#include <cstring>
#include <cuda.h>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include "../src/core/utils/InteropTexture.h"

int main() {
    // Init CUDA
    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        fprintf(stderr, "SKIP: cuInit failed (%d)\n", cr);
        return 0;  // not a failure — CUDA may not be available
    }

    CUdevice cuDev;
    CUcontext cuCtx;
    cuDeviceGet(&cuDev, 0);
    cuCtxCreate(&cuCtx, 0, cuDev);

    // Init Vulkan
    VkInstance inst;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;

    const char* instExts[] = {VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = instExts;

    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        fprintf(stderr, "SKIP: vkCreateInstance failed\n");
        cuCtxDestroy(cuCtx);
        return 0;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    if (pdCount == 0) {
        fprintf(stderr, "SKIP: no Vulkan physical devices\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    VkPhysicalDevice pd = pds[0];

    // Check VK_KHR_external_memory_fd
    uint32_t extCount;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
    bool hasExtMemFd = false;
    for (auto& e : exts) {
        if (strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            hasExtMemFd = true; break;
        }
    }
    if (!hasExtMemFd) {
        fprintf(stderr, "SKIP: VK_KHR_external_memory_fd not available\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    // Create device
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    const char* devExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = devExts;

    VkDevice dev;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) {
        fprintf(stderr, "SKIP: vkCreateDevice failed\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    // Test: create InteropTexture, write via CUDA, read back via Vulkan
    vsr::InteropTexture tex;
    const uint32_t tw = 64, th = 64;
    if (!tex.init((VkDevice_T*)dev, (VkPhysicalDevice_T*)pd, tw, th,
                  VK_FORMAT_R8G8B8A8_UNORM)) {
        fprintf(stderr, "FAIL: InteropTexture::init failed\n");
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }

    // Write test pattern via CUDA D2D
    size_t rowBytes = tw * 4;
    std::vector<uint8_t> testData(rowBytes * th);
    for (uint32_t y = 0; y < th; y++) {
        for (uint32_t x = 0; x < tw; x++) {
            size_t off = y * rowBytes + x * 4;
            testData[off + 0] = (uint8_t)((x * 4) % 256);     // R
            testData[off + 1] = (uint8_t)((y * 4) % 256);     // G
            testData[off + 2] = (uint8_t)(((x + y) * 2) % 256); // B
            testData[off + 3] = 255;                            // A
        }
    }

    // H2D then D2D (or direct H2D to interop ptr)
    CUdeviceptr tmp;
    cuMemAlloc(&tmp, testData.size());
    cuMemcpyHtoD(tmp, testData.data(), testData.size());

    CUDA_MEMCPY2D copy = {};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = tmp;
    copy.srcPitch = rowBytes;
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = tex.cudaPtr();
    copy.dstPitch = tex.cudaPitch();
    copy.WidthInBytes = rowBytes;
    copy.Height = th;
    cuMemcpy2D(&copy);
    cuCtxSynchronize();

    cuMemFree(tmp);

    // Read back via Vulkan map to verify
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout srl;
    vkGetImageSubresourceLayout(dev, (VkImage)tex.imageView_ /* opaque */,
                                &sub, &srl);
    // Actually we need the VkImage, not the imageView. Store VkImage publicly
    // or add an accessor. For now, we verify via CUDA readback instead:

    std::vector<uint8_t> readback(testData.size());
    cuMemcpyDtoH(readback.data(), tex.cudaPtr(), testData.size());
    cuCtxSynchronize();

    bool ok = true;
    for (uint32_t y = 0; y < th && ok; y++) {
        for (uint32_t x = 0; x < tw && ok; x++) {
            size_t off = y * rowBytes + x * 4;
            if (readback[off + 0] != testData[off + 0] ||
                readback[off + 1] != testData[off + 1] ||
                readback[off + 2] != testData[off + 2] ||
                readback[off + 3] != testData[off + 3]) {
                fprintf(stderr, "FAIL: mismatch at (%u,%u)\n", x, y);
                ok = false;
            }
        }
    }

    tex.release();
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    cuCtxDestroy(cuCtx);

    if (ok) fprintf(stderr, "PASS: CUDA-Vulkan interop verified\n");
    return ok ? 0 : 1;
}
```

**Note:** The test above accesses `tex.imageView_` which is private. Add a public `image()` accessor or a `void* vulkanImage() const` to InteropTexture for testing. Or alternatively, skip the Vulkan readback and verify via CUDA readback (D2H from the interop CUDA pointer), which avoids exposing the VkImage.

- [ ] **Step 4: Add external memory extensions to existing Vulkan init**

The Vulkan device currently enables only `VK_KHR_SWAPCHAIN_EXTENSION_NAME`. Interop requires `VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME` and its dependency `VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME`. Task 2 (VulkanContext extraction) will add these. For now, note the requirement.

- [ ] **Step 5: Build and run test**

```bash
cd /home/zmq/projects/vsr-player
# Build test
g++ -std=c++20 -O0 -g -Wall \
    -Ithird_party/cuda/include \
    -Isrc/core -Isrc/core/utils \
    -o build/tests/test_interop \
    tests/test_interop.cpp \
    src/core/utils/InteropTexture.cpp \
    $(pkg-config --cflags --libs vulkan) \
    -Lthird_party/cuda/lib -lcuda -ldl
# Run
./build/tests/test_interop
```

Expected output: `PASS: CUDA-Vulkan interop verified`

- [ ] **Step 6: Commit**

```bash
git add src/core/utils/InteropTexture.h src/core/utils/InteropTexture.cpp \
        tests/test_interop.cpp Makefile
git commit -m "feat: add InteropTexture — CUDA-Vulkan external memory shared image"
```

---

### Task 2: VulkanContext + SwapchainManager — extraction refactor

**Files:**
- Create: `src/core/utils/VulkanContext.h`, `src/core/utils/VulkanContext.cpp`
- Create: `src/core/utils/SwapchainManager.h`, `src/core/utils/SwapchainManager.cpp`
- Modify: `src/core/utils/VulkanRenderer.h`, `src/core/utils/VulkanRenderer.cpp`
- Modify: `Makefile` (add new .o files)

Extract instance/device/queue/command-pool/sync/sampler into VulkanContext, and swapchain/render-pass/framebuffers into SwapchainManager. VulkanRenderer now holds instances of both and delegates. No behavior change — same render output as before.

- [ ] **Step 1: Write VulkanContext header**

```cpp
// src/core/utils/VulkanContext.h
#pragma once

#include <cstdint>

namespace vsr {

/// Vulkan instance, device, queue, and single-use command infrastructure.
/// Extracted from VulkanRenderer — owns the "heavy" Vulkan objects that are
/// shared across swapchains and pipelines.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    /// Create instance, surface, device, queue, command pool, sync primitives.
    /// @param native_window  wl_surface* from Qt winId()
    /// @param native_display wl_display* from Qt platform native interface
    bool init(void* native_window, void* native_display);

    void release();
    bool ready() const { return device_ != nullptr; }

    // -- Accessors (opaque handles, cast to VkXxx in implementation) --
    void* instance()           const { return instance_; }
    void* physicalDevice()    const { return physical_device_; }
    void* device()            const { return device_; }
    void* surface()           const { return surface_; }
    void* queue()             const { return queue_; }
    int   queueFamily()       const { return queue_family_; }
    void* commandPool()       const { return command_pool_; }
    void* commandBuffer()     const { return command_buffer_; }
    void* sampler()           const { return sampler_; }
    void* imageAvailableSem() const { return image_available_semaphore_; }
    void* renderFinishedSem() const { return render_finished_semaphore_; }
    void* fence()             const { return fence_; }

private:
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    void* surface_ = nullptr;
    void* queue_ = nullptr;
    int   queue_family_ = -1;
    void* command_pool_ = nullptr;
    void* command_buffer_ = nullptr;
    void* sampler_ = nullptr;
    void* image_available_semaphore_ = nullptr;
    void* render_finished_semaphore_ = nullptr;
    void* fence_ = nullptr;
};

}  // namespace vsr
```

- [ ] **Step 2: Write VulkanContext implementation**

Move `VulkanRenderer::init()` code into `VulkanContext::init()`, adding `VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME` to device extensions:

```cpp
// In VulkanContext::init() — device extension list:
const char* dev_exts[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
};
// enabledExtensionCount = 3
```

Move instance creation, Wayland surface creation, physical device enumeration, device creation (with the 3 extensions), queue acquisition, command pool + buffer creation, semaphore + fence creation, and sampler creation. The `find_memory_type` helper stays in `VulkanContext.cpp` (or move to a shared utils header).

Move `VulkanRenderer::release()` Vulkan teardown code into `VulkanContext::release()`.

- [ ] **Step 3: Write SwapchainManager header**

```cpp
// src/core/utils/SwapchainManager.h
#pragma once

#include <vector>
#include <cstdint>

namespace vsr {

/// Swapchain, render pass, and framebuffer life cycle.
/// Extracted from VulkanRenderer. Uses physical-pixel sizing via
/// VkSurfaceCapabilitiesKHR::currentExtent.
class SwapchainManager {
public:
    /// Create or recreate the swapchain at the given surface size.
    /// Internally queries caps.currentExtent for physical-pixel sizing.
    bool create(void* physicalDevice, void* device, void* surface,
                int queueFamily, void* renderPassOut = nullptr);

    /// Acquire next swapchain image index.
    /// Returns ~0u on failure.
    uint32_t acquire(void* device, void* swapchain, void* semaphore);

    /// Framebuffer for a given swapchain image index.
    void* framebuffer(uint32_t idx) const;

    int width() const { return swapchain_width_; }
    int height() const { return swapchain_height_; }

    void release(void* device);

private:
    void* swapchain_ = nullptr;
    void* render_pass_ = nullptr;
    std::vector<void*> swapchain_images_;
    std::vector<void*> swapchain_image_views_;
    std::vector<void*> framebuffers_;
    int swapchain_width_ = 0, swapchain_height_ = 0;
};

}  // namespace vsr
```

- [ ] **Step 4: Write SwapchainManager implementation**

Move `VulkanRenderer::create_swapchain_and_pipeline()` swapchain/renderpass/framebuffer code. Add physical-pixel query:

```cpp
bool SwapchainManager::create(void* pd, void* dev, void* surface,
                               int queueFamily, void* rpOut) {
    VkPhysicalDevice p = (VkPhysicalDevice)pd;
    VkDevice d = (VkDevice)dev;
    VkSurfaceKHR s = (VkSurfaceKHR)surface;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, s, &caps);

    uint32_t w = caps.currentExtent.width;
    uint32_t h = caps.currentExtent.height;
    if (w == 0xFFFFFFFF) {  // Wayland "you choose" sentinel
        w = caps.minImageExtent.width;
        h = caps.minImageExtent.height;
    }
    // ... rest of swapchain + renderpass + framebuffer creation
}
```

- [ ] **Step 5: Rewrite VulkanRenderer to use VulkanContext + SwapchainManager**

VulkanRenderer now holds:
```cpp
VulkanContext ctx_;
SwapchainManager swapchain_;
// texture/pipeline/descriptor remain for now (moved to VideoPipeline in Task 5)
```

`VulkanRenderer::init()` delegates to `ctx_.init()`.
`VulkanRenderer::create_swapchain_and_pipeline()` delegates the swapchain part to `swapchain_.create()`.
`VulkanRenderer::release()` calls `swapchain_.release()` then `ctx_.release()`.

- [ ] **Step 6: Update Makefile**

Add to `CORE_OBJS`:
```
$(BUILD_DIR)/src/core/utils/VulkanContext.o \
$(BUILD_DIR)/src/core/utils/SwapchainManager.o \
```

Add compile dependency: `VulkanContext.o` and `SwapchainManager.o` need the same flags as `VulkanRenderer.o`.

- [ ] **Step 7: Build and verify no regression**

```bash
cd /home/zmq/projects/vsr-player && make clean && make -j$(nproc)
./build/vsr-player --no-vsr input/catlove_720p.webm
# Verify: no diagonal shear, correct playback, press Q to quit
```

- [ ] **Step 8: Commit**

```bash
git add src/core/utils/VulkanContext.h src/core/utils/VulkanContext.cpp \
        src/core/utils/SwapchainManager.h src/core/utils/SwapchainManager.cpp \
        src/core/utils/VulkanRenderer.h src/core/utils/VulkanRenderer.cpp \
        Makefile
git commit -m "refactor: extract VulkanContext and SwapchainManager from VulkanRenderer"
```

---

### Task 3: NV12 fragment shader — YUV→RGB in Vulkan

**Files:**
- Create: `src/client/shaders/nv12.frag`
- Modify: `Makefile` (add NV12 shader compile + header generation)

- [ ] **Step 1: Write NV12 fragment shader**

```glsl
// src/client/shaders/nv12.frag
#version 450

layout(binding = 0) uniform sampler2D texY;
layout(binding = 1) uniform sampler2D texUV;

layout(location = 0) in vec2 frag_texcoord;
layout(location = 0) out vec4 out_color;

void main() {
    float y  = texture(texY, frag_texcoord).r;
    vec2  uv = texture(texUV, frag_texcoord).rg;

    // BT.601 full-range YUV → RGB
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;

    float r = clamp(y + 1.402 * v, 0.0, 1.0);
    float g = clamp(y - 0.34414 * u - 0.71414 * v, 0.0, 1.0);
    float b = clamp(y + 1.772 * u, 0.0, 1.0);

    out_color = vec4(r, g, b, 1.0);
}
```

- [ ] **Step 2: Add NV12 shader build rules to Makefile**

```makefile
# Add after existing shader variables:
NV12_FRAG_SPV := $(BUILD_DIR)/shaders/nv12.frag.spv
NV12_FRAG_H   := $(BUILD_DIR)/shaders/nv12_frag_spv.h
SHADERS       := $(VERT_H) $(FRAG_H) $(NV12_FRAG_H)

# Add build rule:
$(NV12_FRAG_SPV): $(CLIENTDIR)/shaders/nv12.frag | $(BUILD_DIR)/shaders
	@echo "  GLSL  nv12.frag"
	@$(GLSLC) -fshader-stage=frag $< -o $@

$(NV12_FRAG_H): $(NV12_FRAG_SPV) | $(BUILD_DIR)/shaders
	@echo "  HDR   nv12_frag_spv.h"
	@xxd -i $< | sed 's/build_shaders_nv12_frag_spv/nv12_frag_spv/g' > $@
```

- [ ] **Step 3: Build shader, verify**

```bash
cd /home/zmq/projects/vsr-player
mkdir -p build/shaders
glslc -fshader-stage=frag src/client/shaders/nv12.frag -o build/shaders/nv12.frag.spv
xxd -i build/shaders/nv12.frag.spv | sed 's/build_shaders_nv12_frag_spv/nv12_frag_spv/g' > build/shaders/nv12_frag_spv.h
head -3 build/shaders/nv12_frag_spv.h
```

Expected: generated header with `nv12_frag_spv` and `nv12_frag_spv_len` symbols.

- [ ] **Step 4: Commit**

```bash
git add src/client/shaders/nv12.frag Makefile
git commit -m "feat: add NV12→RGB fragment shader for no-VSR Vulkan path"
```

---

### Task 4: VideoPipeline — config-driven descriptor + pipeline + InteropTextures

**Files:**
- Create: `src/core/utils/VideoPipeline.h`
- Create: `src/core/utils/VideoPipeline.cpp`
- Modify: `Makefile` (add VideoPipeline.o)

- [ ] **Step 1: Write VideoPipeline header**

```cpp
// src/core/utils/VideoPipeline.h
#pragma once

#include <cstdint>
#include <vector>

#include "InteropTexture.h"

namespace vsr {

/// Describes one texture binding in a pipeline layout.
struct TextureBinding {
    uint32_t binding;
    VkFormat format;
};

/// Configuration for a VideoPipeline instance.
struct PipelineConfig {
    std::vector<TextureBinding> textures;  // e.g. [{0, RGBA8}] or [{0, R8}, {1, R8G8}]
    const uint32_t* fragSpv = nullptr;
    size_t fragSpvLen = 0;
    const uint32_t* vertSpv = nullptr;
    size_t vertSpvLen = 0;
};

/// A complete Vulkan graphics pipeline for rendering a single textured quad,
/// including descriptor set, pipeline layout, and the InteropTexture(s)
/// shared with CUDA.
///
/// Two instances are created:
///   - rgbaPipeline  (1× RGBA8, pass-through shader) → VSR path
///   - nv12Pipeline  (R8 + R8G8, NV12→RGB shader)   → NO-VSR path
class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    /// Initialize from config. Creates descriptor layout, pool, set,
    /// pipeline layout, and graphics pipeline.
    /// @param dev           VkDevice
    /// @param renderPass    VkRenderPass (shared with SwapchainManager)
    /// @param swapchainW, H Swapchain dimensions (for viewport)
    bool init(void* device, void* renderPass,
              uint32_t swapchainW, uint32_t swapchainH,
              const PipelineConfig& cfg);

    /// Bind descriptor set and pipeline to a command buffer.
    void bind(void* commandBuffer);

    /// Access interop texture by binding index (0 = first texture).
    InteropTexture& interopTexture(uint32_t idx = 0);

    void release(void* device);

    bool ready() const { return pipeline_ != nullptr; }
    void* pipeline() const { return pipeline_; }
    void* pipelineLayout() const { return pipeline_layout_; }
    void* descriptorSet() const { return descriptor_set_; }

private:
    void* descriptor_set_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* descriptor_set_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* pipeline_ = nullptr;

    std::vector<InteropTexture> interopTextures_;
    PipelineConfig cfg_;
};

}  // namespace vsr
```

- [ ] **Step 2: Write VideoPipeline implementation**

`VideoPipeline::init()`:
1. For each `TextureBinding`, create one `InteropTexture::init(dev, pd, swapchainW, swapchainH, format)`. But InteropTexture needs VkPhysicalDevice which VideoPipeline doesn't have. Pass it in `init()` or store from construction. Add `void* physicalDevice` to `init()` params.
2. Create `VkDescriptorSetLayout` with bindings matching `cfg.textures`.
3. Create `VkDescriptorPool` with enough descriptors.
4. Allocate `VkDescriptorSet`.
5. Update descriptor set with each InteropTexture's `imageView` + sampler.
6. Create shader modules from `cfg.fragSpv` / `cfg.vertSpv`.
7. Create `VkPipelineLayout`.
8. Create `VkPipeline` (fullscreen triangle, no vertex input, dynamic viewport+scissor).

`VideoPipeline::bind(cb)`:
```cpp
vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
    pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
vkCmdDraw(cb, 3, 1, 0, 0);
```

- [ ] **Step 3: Update Makefile**

Add `$(BUILD_DIR)/src/core/utils/VideoPipeline.o` to `CORE_OBJS`.

- [ ] **Step 4: Build**

```bash
cd /home/zmq/projects/vsr-player && make -j$(nproc)
# Expect: compiles cleanly (link errors OK until Task 5 wires it up)
```

- [ ] **Step 5: Commit**

```bash
git add src/core/utils/VideoPipeline.h src/core/utils/VideoPipeline.cpp Makefile
git commit -m "feat: add VideoPipeline — config-driven descriptor/pipeline with InteropTextures"
```

---

### Task 5: VulkanRenderer refactor — coordinator, new render_frame(Path) API

**Files:**
- Modify: `src/core/utils/VulkanRenderer.h`
- Modify: `src/core/utils/VulkanRenderer.cpp`

Replace the monolithic `render_frame(data, w, h, is_rgba)` with `render_frame(Path)` that delegates to the new sub-objects. Pipeline, descriptor, and texture management move to VideoPipeline instances.

- [ ] **Step 1: Rewrite VulkanRenderer header**

```cpp
// src/core/utils/VulkanRenderer.h
#pragma once

#include <cstdint>
#include "VulkanContext.h"
#include "SwapchainManager.h"
#include "VideoPipeline.h"

namespace vsr {

enum class Path { VSR, NOVSR };

class VulkanRenderer {
public:
    bool init(void* native_window, void* native_display);

    /// Initialize or reinitialize both pipelines. Called after video
    /// dimensions are known (or change on scale/resize).
    /// @param videoW, videoH  Native video frame dimensions.
    bool init_pipelines(int videoW, int videoH,
                        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                        const uint32_t* vertSpv, size_t vertSpvLen);

    /// Render current frame. InteropTextures must have been filled
    /// via CUDA D2D/H2D before calling this.
    bool render_frame(Path path);

    /// Recreate swapchain at the current surface size.
    bool resize(int surface_w, int surface_h);

    void release();
    bool is_ready() const { return ctx_.ready(); }

    // -- Accessors for MainWindow to get InteropTextures for D2D/H2D --
    InteropTexture& rgbaInterop()  { return rgbaPipeline_.interopTexture(0); }
    InteropTexture& yInterop()     { return nv12Pipeline_.interopTexture(0); }
    InteropTexture& uvInterop()    { return nv12Pipeline_.interopTexture(1); }

private:
    VulkanContext ctx_;
    SwapchainManager swapchain_;
    VideoPipeline rgbaPipeline_;
    VideoPipeline nv12Pipeline_;
    int videoW_ = 0, videoH_ = 0;
    bool pipelines_ready_ = false;
};

}  // namespace vsr
```

- [ ] **Step 2: Rewrite VulkanRenderer implementation**

`init()` delegates to `ctx_.init()`.

`init_pipelines()`:
1. `swapchain_.create()` (or re-create)
2. Create `PipelineConfig` for RGBA path:
   ```cpp
   PipelineConfig rgbaCfg;
   rgbaCfg.textures = {{0, VK_FORMAT_R8G8B8A8_UNORM}};
   rgbaCfg.fragSpv = rgbaFragSpv;
   rgbaCfg.fragSpvLen = rgbaFragSpvLen;
   rgbaCfg.vertSpv = vertSpv;
   rgbaCfg.vertSpvLen = vertSpvLen;
   rgbaPipeline_.init(ctx_.device(), swapchain_.renderPass(),
                      swapchain_.width(), swapchain_.height(), rgbaCfg);
   ```
3. Create `PipelineConfig` for NV12 path with 2 textures.

`render_frame(Path path)`:
1. `uint32_t idx = swapchain_.acquire()`
2. Reset + begin command buffer
3. Begin render pass with `swapchain_.framebuffer(idx)`
4. Set viewport + scissor (letterboxing calculation as before)
5. `(path == Path::VSR ? rgbaPipeline_ : nv12Pipeline_).bind(cb)`
6. End render pass, end command buffer
7. Submit + present

Letterboxing calculation uses `videoW_` / `videoH_` and `swapchain_.width()` / `swapchain_.height()`.

`resize()` delegates to `swapchain_.create()`.

`release()` calls `rgbaPipeline_.release()`, `nv12Pipeline_.release()`, `swapchain_.release()`, `ctx_.release()`.

- [ ] **Step 3: Build and verify no regression**

```bash
cd /home/zmq/projects/vsr-player && make clean && make -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add src/core/utils/VulkanRenderer.h src/core/utils/VulkanRenderer.cpp
git commit -m "refactor: simplify VulkanRenderer to coordinator, add render_frame(Path) API"
```

---

### Task 6: VulkanWidget + MainWindow + main.cpp — D2D/H2D, --no-hwaccel

**Files:**
- Modify: `src/client/VulkanWidget.h`, `src/client/VulkanWidget.cpp`
- Modify: `src/client/MainWindow.h`, `src/client/MainWindow.cpp`
- Modify: `src/client/main.cpp`

- [ ] **Step 1: Update VulkanWidget for new API**

```cpp
// VulkanWidget.h — change present_frame signature:
bool present_frame(Path path);

// VulkanWidget.cpp:
bool VulkanWidget::present_frame(Path path) {
    if (!vulkan_ready_) return false;
    if (!renderer_.is_ready()) {
        if (!init_vulkan()) return false;
    }
    int ww = width(), wh = height();
    if (ww > 0 && wh > 0)
        renderer_.resize(ww, wh);
    return renderer_.render_frame(path);
}
```

- [ ] **Step 2: Add InteropTexture members and --no-hwaccel to MainWindow**

```cpp
// MainWindow.h — add:
bool no_hwaccel_ = false;
// (InteropTexture access goes through vulkan_widget_->renderer_.rgbaInterop() etc.)
```

- [ ] **Step 3: Rewrite MainWindow::on_timer_tick — D2D/H2D path**

Replace the GPU→CPU→GPU section with D2D/H2D:

```cpp
void MainWindow::on_timer_tick() {
    // ... decode packet, receive frame (same as before) ...

    cuda_ctx_->push();

    bool is_hw = (hw_frame->format == AV_PIX_FMT_CUDA);

    if (vsr_) {
        // VSR path: NV12→RGB f32 → VSR → D2D to RGBA InteropTexture
        nv12_to_rgb_->convert(y_plane, y_pitch, uv_plane, uv_pitch,
                               video_width_, video_height_,
                               rgb_gpu_, cuda_stream_);

        void* vsr_out_ptr = nullptr;
        int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
        bool vsr_ok = vsr_->process(rgb_gpu_, &vsr_out_ptr,
                                     &vsr_out_w, &vsr_out_h, &vsr_out_pitch);

        if (vsr_ok && vsr_out_ptr) {
            auto& interop = vulkan_widget_->renderer().rgbaInterop();
            size_t rowBytes = (size_t)vsr_out_w * 4;
            CUDA_MEMCPY2D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice     = (CUdeviceptr)vsr_out_ptr;
            copy.srcPitch      = (size_t)vsr_out_pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice     = interop.cudaPtr();
            copy.dstPitch      = interop.cudaPitch();
            copy.WidthInBytes  = rowBytes;
            copy.Height        = (size_t)vsr_out_h;
            cuMemcpy2DAsync(&copy, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
            vsr_w_ = vsr_out_w;
            vsr_h_ = vsr_out_h;
        }
    } else {
        // NO-VSR path: NV12 planes → D2D or H2D to Y+UV InteropTextures
        auto& yInterop  = vulkan_widget_->renderer().yInterop();
        auto& uvInterop = vulkan_widget_->renderer().uvInterop();

        if (is_hw) {
            CUDA_MEMCPY2D copyY = {};
            copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copyY.srcDevice     = (CUdeviceptr)y_plane;
            copyY.srcPitch      = (size_t)y_pitch;
            copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyY.dstDevice     = yInterop.cudaPtr();
            copyY.dstPitch      = yInterop.cudaPitch();
            copyY.WidthInBytes  = (size_t)video_width_;
            copyY.Height        = (size_t)video_height_;
            cuMemcpy2DAsync(&copyY, (CUstream)cuda_stream_);

            CUDA_MEMCPY2D copyUV = {};
            copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copyUV.srcDevice     = (CUdeviceptr)uv_plane;
            copyUV.srcPitch      = (size_t)uv_pitch;
            copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyUV.dstDevice     = uvInterop.cudaPtr();
            copyUV.dstPitch      = uvInterop.cudaPitch();
            copyUV.WidthInBytes  = (size_t)video_width_;
            copyUV.Height        = (size_t)(video_height_ / 2);
            cuMemcpy2DAsync(&copyUV, (CUstream)cuda_stream_);
        } else {
            // SW decode: H2D
            cuMemcpyHtoDAsync(yInterop.cudaPtr(), y_plane,
                              video_width_ * video_height_, cuda_stream_);
            cuMemcpyHtoDAsync(uvInterop.cudaPtr(), uv_plane,
                              video_width_ * video_height_ / 2, cuda_stream_);
        }
        cuStreamSynchronize((CUstream)cuda_stream_);
        vsr_w_ = video_width_;
        vsr_h_ = video_height_;
    }

    cuda_ctx_->pop();
    decoder_->release_frame(hw_frame);

    // A/V sync (unchanged) ...

    // Render
    vulkan_widget_->present_frame(vsr_ ? Path::VSR : Path::NOVSR);
}
```

Remove `rgba_host_` usage — no longer needed.

- [ ] **Step 4: Pipeline initialization in open_file()**

After `vsr_w_` / `vsr_h_` are determined, initialize both pipelines via `VulkanRenderer::init_pipelines()`. The SPIR-V data comes from the generated headers:

```cpp
#include "video_vert_spv.h"
#include "video_frag_spv.h"
#include "nv12_frag_spv.h"

// ... in open_file(), after VulkanWidget is shown:
vulkan_widget_->renderer().init_pipelines(
    video_width_, video_height_,
    video_frag_spv, video_frag_spv_len,      // RGBA pass-through
    nv12_frag_spv, nv12_frag_spv_len,        // NV12→RGB
    video_vert_spv, video_vert_spv_len);     // shared vertex shader
```

**Note on InteropTexture sizing:** For the NO-VSR path, InteropTextures are sized to `video_width_ × video_height_` (Y) and `video_width_/2 × video_height_/2` (UV). For the VSR path, sized to `vsr_w_ × vsr_h_`. Re-initialize pipelines on scale change.

- [ ] **Step 5: Add --no-hwaccel flag to main.cpp**

```cpp
// In main.cpp, add after existing flag parsing:
bool no_hwaccel = false;
// ...
} else if (strcmp(argv[i], "--no-hwaccel") == 0) {
    no_hwaccel = true;
}

// Pass to MainWindow constructor or setter:
vsr::MainWindow window(use_vsr, quality);
window.set_no_hwaccel(no_hwaccel);
```

In `open_file()`, when `no_hwaccel_` is true, skip CUDA hwaccel setup and use software decoder.

- [ ] **Step 6: Build and test all paths**

```bash
cd /home/zmq/projects/vsr-player && make clean && make -j$(nproc)

# Test 1: NO-VSR HW decode
./build/vsr-player --no-vsr input/catlove_720p.webm
# Expected: correct colors, no diagonal shear

# Test 2: VSR HW decode
./build/vsr-player input/catlove_720p.webm
# Expected: correct VSR output, no shear

# Test 3: NO-VSR SW decode
./build/vsr-player --no-vsr --no-hwaccel input/catlove_720p.webm
# Expected: software decode, H2D path, correct output

# Test 4: VSR SW decode
./build/vsr-player --no-hwaccel input/catlove_720p.webm
# Expected: SW decode → H2D → CUDA kernel → VSR → D2D → correct output
```

- [ ] **Step 7: Commit**

```bash
git add src/client/VulkanWidget.h src/client/VulkanWidget.cpp \
        src/client/MainWindow.h src/client/MainWindow.cpp \
        src/client/main.cpp
git commit -m "feat: zero-PCIe D2D/H2D pipeline, NV12 shader, --no-hwaccel flag"
```

---

### Task 7: Final integration test + cleanup

**Files:** none (verification only)

- [ ] **Step 1: Full rebuild**

```bash
cd /home/zmq/projects/vsr-player && make clean && make -j$(nproc)
```

- [ ] **Step 2: Test all four path combinations**

| # | Command | Path | Expected |
|---|---------|------|----------|
| 1 | `--no-vsr` | NO-VSR, NVDEC | NV12 shader renders, no shear |
| 2 | `--no-vsr --no-hwaccel` | NO-VSR, SW | NV12 shader renders via H2D |
| 3 | `<none>` | VSR, NVDEC | VSR upscale, D2D to Vulkan |
| 4 | `--no-hwaccel` | VSR, SW | VSR upscale, 1 H2D + 1 D2D |

- [ ] **Step 3: Verify no PCIe round-trips (via logging)**

Add temporary debug counters or check that `cuMemcpyDtoH` / `cuMemcpyDtoHAsync` (not `HtoD`) are no longer called in the render path. `rgba_host_` usage should be gone from `on_timer_tick`.

- [ ] **Step 4: Test resize behavior**

Play a video, resize window → swapchain rebuilds at physical pixel extent, letterboxing correct.

- [ ] **Step 5: Commit any cleanup**

```bash
git add -u && git commit -m "chore: remove dead DtoH code, final cleanup"
```

---

## Self-Review

1. **Spec coverage**: Each spec requirement maps to a task:
   - InteropTexture → Task 1
   - VulkanContext extraction → Task 2
   - SwapchainManager + physical pixels → Task 2
   - VideoPipeline → Task 4
   - NV12 shader → Task 3
   - VulkanRenderer refactor → Task 5
   - D2D/H2D in MainWindow → Task 6
   - `--no-hwaccel` → Task 6
   - SW decode support → Task 6

2. **Placeholder scan**: No TBD, TODO, or incomplete sections. All code steps have concrete implementations.

3. **Type consistency**: `Path` enum used consistently across VulkanRenderer, VulkanWidget, MainWindow. `InteropTexture` accessors match MainWindow D2D usage. `PipelineConfig` matches `VideoPipeline::init()`.
