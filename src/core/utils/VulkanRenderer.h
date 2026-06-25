#pragma once

#include <atomic>
#include <cstdint>
#include "VideoPipeline.h"
#include "../api/Player.h"  // Path enum, IVulkanContext

namespace vsr {

/// Two-stage renderer: init_pipelines (once) + reconfigure_textures (per-file/scale).
///
/// Texture mutation happens on the worker thread, protected by a Render Gate
/// (lock-free worker↔render handshake). The gate ensures the render thread
/// isn't recording commands referencing old textures before they're destroyed.
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    bool init_pipelines(
        IVulkanContext& vk,
        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
        const uint32_t* vertSpv, size_t vertSpvLen);

    /// Record video draw into external command buffer (render thread).
    void record_to_cb(void* cb, int extentW, int extentH, Path path);

    bool resize(int w, int h);

    void release(IVulkanContext& vk);

    void set_running_flag(const std::atomic<bool>* flag) { running_flag_ = flag; }

    /// Scale change: rebuild RGBA texture (worker thread).
    bool reconfigure_scale(int videoW, int videoH, int newScale,
                           IVulkanContext& vk);

    /// Video switch: rebuild ALL textures (worker thread).
    bool reconfigure_all_textures(int videoW, int videoH, int scale,
                                  IVulkanContext& vk);

    bool is_ready() const { return pipelines_ready_; }
    int mutation_gate() const { return mutation_gate_.load(); }
    int render_in_frame() const { return render_in_frame_.load(); }
    int frames_since_mutation() const { return frames_since_mutation_.load(); }

    InteropTexture& rgbaInterop() { return rgbaPipeline_.interopTexture(0); }
    InteropTexture& yInterop()    { return nv12Pipeline_.interopTexture(0); }
    InteropTexture& uvInterop()   { return nv12Pipeline_.interopTexture(1); }

private:
    VideoPipeline rgbaPipeline_;
    VideoPipeline nv12Pipeline_;
    bool pipelines_ready_ = false;
    int video_w_ = 0, video_h_ = 0;
    int vsr_scale_ = 1;
    int last_widget_w_ = 0, last_widget_h_ = 0;

    void* cached_device_ = nullptr;
    bool textures_need_transition_ = false;

    // ── Render Gate — lock-free worker↔render handshake ──
    // mutation_gate_: 0=normal, 1=worker wants to mutate
    // render_in_frame_: >0 when render thread is inside record_to_cb
    std::atomic<int> mutation_gate_{0};
    std::atomic<int> render_in_frame_{0};
    std::atomic<int> frames_since_mutation_{0};
    const std::atomic<bool>* running_flag_ = nullptr;

    void begin_mutation(IVulkanContext& vk);
    void end_mutation();
};

}  // namespace vsr
