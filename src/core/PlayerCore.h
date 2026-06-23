#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
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

/// Concrete Player implementation — full video playback engine.
///
/// Owns a worker thread that runs the complete pipeline:
/// demux → decode → NV12→RGB → VSR → D2D → Vulkan render → present.
/// The client sends commands via send_command() and receives events
/// via the callback (fired from the worker thread).
class PlayerCore : public Player {
public:
    PlayerCore();
    ~PlayerCore() override;

    // Player interface
    void set_event_callback(EventCallback cb) override;
    void send_command(PlayerCommand cmd) override;
    bool initialize(void* native_window, void* native_display,
                    bool use_vsr, Quality quality) override;
    void shutdown() override;

private:
    // ── Thread & queue ──
    void run_loop();
    bool process_command_nonblock();
    void emit_event(PlayerEvent e);

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    CommandQueue<PlayerCommand> cmd_queue_;
    EventCallback event_cb_;

    // ── Surface handles (set by initialize, used on worker thread) ──
    void* native_window_ = nullptr;
    void* native_display_ = nullptr;

    // ── Config ──
    bool use_vsr_ = true;
    Quality quality_ = Quality::HIGH;
    PlaybackState state_ = PlaybackState::STOPPED;

    // ── Pipeline subsystems ──
    std::unique_ptr<VulkanRenderer> renderer_;
    std::unique_ptr<CUDAContext>   cuda_ctx_;
    std::unique_ptr<Demuxer>       demuxer_;
    std::unique_ptr<Decoder>       decoder_;
    std::unique_ptr<VSRProcessor>  vsr_;
    std::unique_ptr<NV12ToRGB>     nv12_to_rgb_;
    std::unique_ptr<AudioOutput>   audio_;
    bool audio_started_ = false;  // true after first AudioOutput::start()

    // ── GPU resources ──
    float* rgb_gpu_ = nullptr;
    void*  cuda_stream_ = nullptr;

    // ── Video info ──
    int video_w_ = 0, video_h_ = 0;
    double video_fps_ = 0.0;
    int vsr_w_ = 0, vsr_h_ = 0;
    int current_scale_ = 1;
    int64_t frame_count_ = 0;
    int64_t duration_ms_ = 0;
    int64_t last_position_emit_ms_ = 0;
    bool seekable_ = true;

    // ── Window size (pending if set before pipeline ready) ──
    int pending_phys_w_ = 0, pending_phys_h_ = 0;

    // ── Screenshot ──
    bool capture_pending_ = false;
    std::vector<uint8_t> capture_orig_buf_;
    std::vector<uint8_t> capture_vsr_buf_;

    // ── Command handlers ──
    void cmd_load_file(const std::string& path);
    void cmd_resize(int phys_w, int phys_h);
    void cmd_play();
    void cmd_pause();
    void cmd_stop();
    void cmd_seek(int64_t ms);
    void cmd_set_quality(Quality q);
    void cmd_set_scale(int s);
    void cmd_set_volume(double vol);
    void cmd_set_mute(bool mute);

    // ── Pipeline lifecycle ──
    bool build_pipeline(const std::string& path);
    void teardown_pipeline();
    void reconfigure_vsr();

    // ── Frame processing ──
    bool process_one_frame();
};

}  // namespace vsr
