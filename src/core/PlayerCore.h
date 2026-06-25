#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "api/Player.h"
#include "CommandQueue.h"

namespace vsr {

class Demuxer;
class Decoder;
class VSRProcessor;
class AudioOutput;
class VulkanRenderer;
class CUDAContext;
class NV12ToRGB;

// ── TargetState — merged user intent snapshot ──
struct TargetState {
    std::string file;
    int phys_w = 0;
    int phys_h = 0;
    int scale = 0;      // -1=off, 0=auto, 2-4=locked

    void merge(const PlayerCommand& cmd) {
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CmdLoadFile>) {
                file = arg.path;
                phys_w = phys_h = 0;
            } else if constexpr (std::is_same_v<T, CmdResize>) {
                phys_w = arg.phys_w; phys_h = arg.phys_h;
            } else if constexpr (std::is_same_v<T, CmdSetScale>) {
                scale = arg.scale;
            }
        }, cmd);
    }
};

class PlayerCore : public Player {
public:
    PlayerCore();
    ~PlayerCore() override;

    // Player interface
    bool initialize(IVulkanContext* vk,
                    const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                    const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                    const uint32_t* vertSpv, size_t vertSpvLen,
                    int quality = 3,
                    bool no_hwaccel = false) override;
    void send_command(PlayerCommand cmd) override;
    void record_frame(void* cb, int w, int h) override;
    void set_event_callback(EventCallback cb) override;
    void shutdown() override;

private:
    // ── Thread & queue ──
    void run_loop();
    void emit_event(PlayerEvent e);

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    CommandQueue<PlayerCommand> cmd_queue_;
    EventCallback event_cb_;

    // ── Client-owned Vulkan context (borrowed, never destroyed) ──
    IVulkanContext* vk_ = nullptr;

    // ── Config ──
    bool no_hwaccel_ = false;
    int quality_ = 3;               // VFX upscale quality (1-4, default HIGH=3)
    int denoise_quality_ = -1;      // VFX denoise quality (-1=off, 8-11=low-ultra)
    PlaybackState state_ = PlaybackState::STOPPED;

    // ── Pipeline subsystems ──
    std::unique_ptr<VulkanRenderer> renderer_;  // core-owned Vulkan pipelines + textures
    std::unique_ptr<CUDAContext>   cuda_ctx_;
    std::unique_ptr<Demuxer>       demuxer_;
    std::unique_ptr<Decoder>       decoder_;
    std::unique_ptr<VSRProcessor>  vsr_;
    std::unique_ptr<NV12ToRGB>     nv12_to_rgb_;
    std::unique_ptr<AudioOutput>   audio_;
    bool audio_started_ = false;

    // ── GPU resources ──
    float* rgb_gpu_ = nullptr;
    void*  cuda_stream_ = nullptr;
    void*  sw_staging_y_ = nullptr;   // pre-allocated GPU buffers for SW decode H2D
    void*  sw_staging_uv_ = nullptr;

    // ── Video info ──
    int video_w_ = 0, video_h_ = 0;
    double video_fps_ = 0.0;
    double video_time_base_ = 0.0;   // seconds per PTS tick
    int vsr_w_ = 0, vsr_h_ = 0;
    int current_scale_ = 1;
    int user_scale_ = 0;            // -1=off, 0=auto, 2-4=locked
    std::string current_file_;
    int64_t duration_ms_ = 0;
    int64_t last_position_emit_ms_ = 0;
    bool seekable_ = true;

    // ── PTS-based sync (mpv/VLC model: real decoder timestamps) ──
    double current_pts_sec_ = 0.0;   // current frame PTS in seconds
    double last_pts_sec_ = -1.0;     // previous rendered frame PTS
    double playback_speed_ = 1.0;    // display speed (affects pacing only)
    int64_t pts_fallback_counter_ = 0;  // fallback when hw_frame->pts invalid
    std::chrono::steady_clock::time_point last_render_time_;

    // ── Window size ──
    int pending_phys_w_ = 0, pending_phys_h_ = 0;
    int last_phys_w_ = 0, last_phys_h_ = 0;

    // ── Render thread state (read by render thread) ──
    std::atomic<bool> frame_ready_{false};
    Path current_path_ = Path::NOVSR;

    // ── Command coalescing ──
    bool busy_ = false;
    TargetState target_state_;

    // RESIZE debounce — core-side 800ms timer (worker thread)
    int stored_resize_w_ = 0;
    int stored_resize_h_ = 0;
    std::chrono::steady_clock::time_point last_resize_cmd_;

    // ── Screenshot ──
    bool capture_pending_ = false;
    std::vector<uint8_t> capture_orig_buf_;
    std::vector<uint8_t> capture_vsr_buf_;

    // ── Command handlers (one per variant alternative) ──
    void cmd_play();
    void cmd_pause();
    void cmd_stop();
    void cmd_quit();
    void cmd_load_file(const std::string& path);
    void cmd_seek(int64_t ms);
    void cmd_resize(int phys_w, int phys_h);
    void cmd_set_quality(int q);
    void cmd_set_denoise_quality(int d);
    void cmd_set_scale(int s);
    void cmd_set_volume(double vol);
    void cmd_set_mute(bool mute);
    void cmd_set_hwaccel(bool enabled);
    void cmd_set_speed(double speed);
    void cmd_capture();

    // ── Dispatch ──
    void dispatch(const PlayerCommand& cmd);
    bool is_light(const PlayerCommand& cmd) const;

    // ── Coalescing ──
    bool has_pending_work() const;
    void apply();
    bool execute(const TargetState& snapshot);

    // ── Pipeline lifecycle ──
    void teardown_pipeline();                    // QUIT only
    void teardown_file_resources();              // LOAD_FILE: demuxer/decoder/audio/rgb_gpu_
    bool build_file_resources(const std::string& path);
    void reconfigure_vsr();

    // ── Adaptive scale ──
    int compute_adaptive_scale(int phys_w, int phys_h,
                               int video_w, int video_h) const;

    // ── Frame processing ──
    bool process_one_frame();
};

}  // namespace vsr
