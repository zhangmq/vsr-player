#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "api/Player.h"
#include "CommandQueue.h"

namespace vsr {

class ClockManager;
class Demuxer;
class Decoder;
class VSRProcessor;
class Renderer;
class AudioOutput;
class FramePool;

/// Concrete Player implementation — central coordinator.
class PlayerCore : public Player {
public:
    PlayerCore();
    ~PlayerCore() override;

    // Player interface
    void set_event_callback(EventCallback cb) override;
    void send_command(PlayerCommand cmd) override;
    bool initialize(void* vulkan_surface) override;
    void shutdown() override;

private:
    void run_loop();           // Main playback thread
    void process_command(const PlayerCommand& cmd);
    void emit_event(PlayerEvent event);

    EventCallback event_cb_;
    CommandQueue<PlayerCommand> cmd_queue_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    // Subsystems (owned)
    std::unique_ptr<ClockManager> clock_;
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<VSRProcessor> vsr_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<AudioOutput> audio_;
    std::unique_ptr<FramePool> frame_pool_;

    PlaybackState state_ = PlaybackState::STOPPED;
    void* vulkan_surface_ = nullptr;
};

}  // namespace vsr
