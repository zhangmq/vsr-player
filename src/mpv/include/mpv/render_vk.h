/*
 * Vulkan render API for libmpv
 *
 * Allows embedding mpv video rendering into an external Vulkan context.
 * The host provides VkInstance, VkDevice, and per-frame VkImage targets.
 * mpv renders directly into the user's VkImage — no swapchain ownership.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// API type constant for mpv_render_context_create()
#define MPV_RENDER_API_TYPE_VULKAN "vulkan"

// ── Init params (passed once, before mpv_render_context_create) ──────

// Valid values for mpv_render_param.type when api_type is "vulkan":
//   MPV_RENDER_PARAM_VULKAN_INIT_PARAMS — mpv_vulkan_init_params*
//     Must be provided. Sets up the Vulkan device import.
//   MPV_RENDER_PARAM_VULKAN_IMAGE — mpv_vulkan_image*
//     Per-frame render target. Provided in mpv_render_context_render().
//   MPV_RENDER_PARAM_WL_DISPLAY — wl_display*
//     Required on Wayland for VAAPI/dmabuf interop.
//   MPV_RENDER_PARAM_X11_DISPLAY — Display*
//     Required on X11 for VAAPI/dmabuf interop.

typedef struct mpv_vulkan_init_params {
    // The externally-created Vulkan instance.
    VkInstance instance;

    // The selected physical device.
    VkPhysicalDevice physical_device;

    // The externally-created logical device.
    VkDevice device;

    // Function pointer resolver. libplacebo uses this to load all
    // Vulkan functions. Required.
    PFN_vkGetInstanceProcAddr get_proc_addr;

    // Queue family indices for the queues mpv will use.
    // At minimum, graphics_queue_count must be > 0.
    int graphics_queue_family;
    int graphics_queue_index;
    int graphics_queue_count;

    // Optional: dedicated compute queue. Set count=0 if not available.
    int compute_queue_family;
    int compute_queue_index;
    int compute_queue_count;

    // Optional: dedicated transfer queue. Set count=0 if not available.
    int transfer_queue_family;
    int transfer_queue_index;
    int transfer_queue_count;

    // Enabled device extensions (null-terminated string array).
    const char *const *extensions;
    int num_extensions;

    // Enabled physical device features (VkPhysicalDeviceFeatures2 chain).
    // NULL means use the default libplacebo required features.
    const VkPhysicalDeviceFeatures2 *features;

    // Optional debug flag.
    bool debug;
} mpv_vulkan_init_params;

// ── Per-frame render target ──────────────────────────────────────────

typedef struct mpv_vulkan_image {
    // The VkImage to render into. Must be in a layout compatible with
    // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL or
    // VK_IMAGE_LAYOUT_GENERAL, and created with
    // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT.
    VkImage image;

    // Image format and dimensions.
    VkFormat format;
    int width;
    int height;

    // ── Synchronization ──
    // Semaphore that mpv must WAIT on before rendering.
    // This signals when the image is ready for mpv to write into.
    // Set to VK_NULL_HANDLE if no wait is needed.
    VkSemaphore wait_semaphore;
    uint64_t wait_semaphore_value;  // for timeline semaphores

    // Semaphore that mpv must SIGNAL when rendering is complete.
    // The host waits on this before reading/presenting the image.
    // Set to VK_NULL_HANDLE if no signal is needed.
    VkSemaphore signal_semaphore;
    uint64_t signal_semaphore_value; // for timeline semaphores
} mpv_vulkan_image;

#ifdef __cplusplus
}
#endif

#endif // MPV_CLIENT_API_RENDER_VK_H_
