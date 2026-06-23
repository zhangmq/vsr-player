/// Headless test for InteropTexture (CUDA-Vulkan external memory interop).
///
/// Flow:
///   1. Init CUDA and Vulkan (no Wayland surface needed)
///   2. Check VK_KHR_external_memory_fd device extension
///   3. Create InteropTexture (64x64 RGBA8)
///   4. Write test pattern via CUDA (cuMemcpy2D → interop texture)
///   5. Read back via CUDA (cuMemcpyDtoH)
///   6. Verify pattern
///   7. Print PASS / FAIL / SKIP
///
/// Build:
///   make test_interop   (or see Makefile for manual command)
///
/// Run:
///   ./build/tests/test_interop

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <cuda.h>

#include "InteropTexture.h"

// ── Main ────────────────────────────────────────────────────────────

int main() {
    printf("=== Test: InteropTexture (CUDA-Vulkan external memory) ===\n\n");

    // ──────────────────────────────────────────────────────────────────
    // 1. Init CUDA
    // ──────────────────────────────────────────────────────────────────
    CUresult cuRes = cuInit(0);
    if (cuRes != CUDA_SUCCESS) {
        printf("SKIP: cuInit failed (%d) — CUDA not available\n", cuRes);
        return 0;
    }

    CUdevice cuDev;
    cuRes = cuDeviceGet(&cuDev, 0);
    if (cuRes != CUDA_SUCCESS) {
        printf("SKIP: cuDeviceGet(0) failed (%d)\n", cuRes);
        return 0;
    }

    char cuName[256];
    cuDeviceGetName(cuName, sizeof(cuName), cuDev);

    CUcontext cuCtx;
    cuRes = cuCtxCreate(&cuCtx, nullptr, 0, cuDev);
    if (cuRes != CUDA_SUCCESS) {
        printf("SKIP: cuCtxCreate failed (%d)\n", cuRes);
        return 0;
    }
    printf("[1] CUDA  — %s\n", cuName);

    // ──────────────────────────────────────────────────────────────────
    // 2. Init Vulkan
    // ──────────────────────────────────────────────────────────────────
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "test_interop";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;

    VkInstance inst;
    VkResult res = vkCreateInstance(&ici, nullptr, &inst);
    if (res != VK_SUCCESS) {
        printf("SKIP: vkCreateInstance failed (%d)\n", res);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    if (pdCount == 0) {
        printf("SKIP: no Vulkan physical devices\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("[2] Vulkan — %s (API %u.%u.%u)\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion));

    // Check for VK_KHR_external_memory_fd device extension
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availExts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount,
                                         availExts.data());

    bool hasExtMem    = false;
    bool hasExtMemFd  = false;
    bool hasDmaBuf    = false;
    for (const auto& e : availExts) {
        if (strcmp(e.extensionName,
                   VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
            hasExtMem = true;
        if (strcmp(e.extensionName,
                   VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0)
            hasExtMemFd = true;
        if (strcmp(e.extensionName,
                   VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0)
            hasDmaBuf = true;
    }

    if (!hasExtMem || !hasExtMemFd) {
        printf("SKIP: VK_KHR_external_memory_fd not available on device\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    if (hasDmaBuf)
        printf("[3] Exts  — VK_KHR_external_memory_fd + DMA_BUF available\n");
    else
        printf("[3] Exts  — VK_KHR_external_memory_fd available\n");

    // Find a graphics queue family
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfps.data());

    int qfIdx = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qfIdx = (int)i;
            break;
        }
    }
    if (qfIdx < 0) {
        printf("SKIP: no graphics queue\n");
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }

    // Create device with external memory extensions
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = (uint32_t)qfIdx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    const char* devExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
    };

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = devExts;

    VkDevice dev;
    res = vkCreateDevice(pd, &dci, nullptr, &dev);
    if (res != VK_SUCCESS) {
        printf("SKIP: vkCreateDevice failed (%d)\n", res);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 0;
    }
    printf("[4] Device — Vulkan logical device created\n");

    // ──────────────────────────────────────────────────────────────────
    // 3. Create InteropTexture
    // ──────────────────────────────────────────────────────────────────
    const uint32_t W = 64, H = 64;

    vsr::InteropTexture tex;
    if (!tex.init((VkDevice_T*)dev, (VkPhysicalDevice_T*)pd,
                  W, H, VK_FORMAT_R8G8B8A8_UNORM)) {
        printf("FAIL: InteropTexture::init\n");
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }

    printf("[5] Tex   — %ux%u pitch=%zu cudaPtr=0x%lx\n",
           tex.width(), tex.height(), tex.cudaPitch(),
           (unsigned long)tex.cudaPtr());

    // ──────────────────────────────────────────────────────────────────
    // 4. Write test pattern via CUDA
    // ──────────────────────────────────────────────────────────────────
    size_t pitch = tex.cudaPitch();

    // Allocate temporary CUDA buffer
    CUdeviceptr tempBuf;
    cuRes = cuMemAlloc(&tempBuf, pitch * H);
    if (cuRes != CUDA_SUCCESS) {
        printf("FAIL: cuMemAlloc (%d)\n", cuRes);
        tex.release();
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }

    // Build host-side pattern (RGBA8, gradient-style)
    // pitch may be larger than W*4 due to alignment padding
    std::vector<uint8_t> pattern(pitch * H);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            size_t off = y * pitch + x * 4;
            pattern[off + 0] = (uint8_t)(x * 4);        // R: horizontal gradient
            pattern[off + 1] = (uint8_t)(y * 4);        // G: vertical gradient
            pattern[off + 2] = (uint8_t)((x + y) * 2);  // B: diagonal blend
            pattern[off + 3] = 255;                      // A: fully opaque
        }
    }

    // Host → temp CUDA buffer
    cuRes = cuMemcpyHtoD(tempBuf, pattern.data(), pitch * H);
    if (cuRes != CUDA_SUCCESS) {
        printf("FAIL: cuMemcpyHtoD (%d)\n", cuRes);
        cuMemFree(tempBuf);
        tex.release();
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }

    // D2D: tempBuf → interop texture (respects pitch)
    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice     = tempBuf;
    cp.srcPitch      = pitch;
    cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstDevice     = tex.cudaPtr();
    cp.dstPitch      = pitch;
    cp.WidthInBytes  = W * 4;
    cp.Height        = H;

    cuRes = cuMemcpy2D(&cp);
    if (cuRes != CUDA_SUCCESS) {
        printf("FAIL: cuMemcpy2D (%d)\n", cuRes);
        cuMemFree(tempBuf);
        tex.release();
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }
    printf("[6] Write — CUDA cuMemcpy2D -> interop texture\n");

    // ──────────────────────────────────────────────────────────────────
    // 5. Read back and verify
    // ──────────────────────────────────────────────────────────────────
    std::vector<uint8_t> readback(pitch * H);
    cuRes = cuMemcpyDtoH(readback.data(), tex.cudaPtr(), pitch * H);
    if (cuRes != CUDA_SUCCESS) {
        printf("FAIL: cuMemcpyDtoH (%d)\n", cuRes);
        cuMemFree(tempBuf);
        tex.release();
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        cuCtxDestroy(cuCtx);
        return 1;
    }

    bool pass = true;
    int checked = 0;
    for (uint32_t y = 0; y < H && pass; y++) {
        for (uint32_t x = 0; x < W; x++) {
            size_t off = y * pitch + x * 4;
            if (readback[off + 0] != pattern[off + 0] ||
                readback[off + 1] != pattern[off + 1] ||
                readback[off + 2] != pattern[off + 2] ||
                readback[off + 3] != pattern[off + 3]) {
                printf("FAIL: mismatch at (%u,%u) "
                       "got {%3d,%3d,%3d,%3d} expected {%3d,%3d,%3d,%3d}\n",
                       x, y,
                       readback[off + 0], readback[off + 1],
                       readback[off + 2], readback[off + 3],
                       pattern[off + 0], pattern[off + 1],
                       pattern[off + 2], pattern[off + 3]);
                pass = false;
            }
            checked++;
        }
    }

    // ──────────────────────────────────────────────────────────────────
    // 6. Cleanup
    // ──────────────────────────────────────────────────────────────────
    cuMemFree(tempBuf);
    tex.release();
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    cuCtxDestroy(cuCtx);

    // ──────────────────────────────────────────────────────────────────
    // 7. Report
    // ──────────────────────────────────────────────────────────────────
    if (pass) {
        printf("[7] Read  — %d pixels verified\n", checked);
        printf("\nPASS: InteropTexture — CUDA write + readback OK\n");
        return 0;
    }
    // pass == false already printed details
    return 1;
}
