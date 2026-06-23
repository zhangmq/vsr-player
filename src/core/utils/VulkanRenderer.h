#pragma once

#include <atomic>
#include <cstdint>
#include "VulkanContext.h"
#include "SwapchainManager.h"
#include "VideoPipeline.h"

namespace vsr {

enum class Path { VSR, NOVSR };

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    bool init(void* native_window, void* native_display);

    /// Initialize pipelines after video dimensions are known.
    /// Creates both RGBA (VSR) and NV12 (NO-VSR) pipelines.
    /// @param videoW, videoH  Native (pre-scale) video frame dimensions.
    /// @param scale  VSR scale factor (1 for 1:1, 2 for 2x, etc.)
    /// @param widgetW, widgetH  Widget pixel dimensions for initial swapchain size.
    bool init_pipelines(int videoW, int videoH, int scale,
                        int widgetW, int widgetH,
                        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                        const uint32_t* vertSpv, size_t vertSpvLen);

    /// Render. InteropTextures must have been filled via CUDA D2D/H2D
    /// before calling this.
    bool render_frame(Path path);

    bool resize(int w, int h);
    void release();
    bool is_ready() const { return ctx_.ready() && pipelines_ready_; }

    // Accessor for MainWindow D2D/H2D copies
    InteropTexture& rgbaInterop() { return rgbaPipeline_.interopTexture(0); }
    InteropTexture& yInterop()    { return nv12Pipeline_.interopTexture(0); }
    InteropTexture& uvInterop()   { return nv12Pipeline_.interopTexture(1); }

    int swapchainWidth()  const { return swapchain_.width(); }
    int swapchainHeight() const { return swapchain_.height(); }

    /// Store SPIR-V shader data for later pipeline (re)creation.
    /// Called once by the engine after initialization.
    void set_shader_data(const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                         const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                         const uint32_t* vertSpv, size_t vertSpvLen);

    /// Init or recreate pipelines using previously stored SPIR-V data.
    bool init_pipelines_with_saved_spv(int videoW, int videoH, int scale,
                                        int widgetW, int widgetH);

    /// Set a shutdown flag pointer.  render_frame() checks this flag
    /// after every finite Vulkan wait — when set, it returns early so the
    /// worker thread can process QUIT and tear down cleanly.
    void set_shutdown_flag(const std::atomic<bool>* flag) {
        shutting_down_ = flag;
    }

private:
    VulkanContext ctx_;
    SwapchainManager swapchain_;
    VideoPipeline rgbaPipeline_;
    VideoPipeline nv12Pipeline_;
    bool pipelines_ready_ = false;
    int video_w_ = 0, video_h_ = 0;
    int vsr_scale_ = 1;
    int last_widget_w_ = 0, last_widget_h_ = 0;

    // Shutdown signal — checked in render_frame() to break out of
    // blocking Vulkan waits. Points to PlayerCore::shutting_down_.
    const std::atomic<bool>* shutting_down_ = nullptr;

    // Saved SPIR-V pointers for pipeline recreation on resize
    const uint32_t* saved_vert_spv_ = nullptr;
    const uint32_t* saved_rgba_frag_spv_ = nullptr;
    const uint32_t* saved_nv12_frag_spv_ = nullptr;
    size_t saved_vert_len_ = 0;
    size_t saved_rgba_frag_len_ = 0;
    size_t saved_nv12_frag_len_ = 0;
};

}  // namespace vsr
