#include "PlayerCore.h"

#include <cassert>

#include "AudioOutput.h"
#include "ClockManager.h"
#include "Decoder.h"
#include "Demuxer.h"
#include "FramePool.h"
#include "Renderer.h"
#include "VSRProcessor.h"

namespace vsr {

PlayerCore::PlayerCore() = default;

PlayerCore::~PlayerCore() {
    shutdown();
}

void PlayerCore::set_event_callback(EventCallback cb) {
    event_cb_ = std::move(cb);
}

void PlayerCore::send_command(PlayerCommand cmd) {
    cmd_queue_.push(std::move(cmd));
}

bool PlayerCore::initialize(void* vulkan_surface) {
    vulkan_surface_ = vulkan_surface;
    // TODO: init subsystems when dependencies are ready
    running_ = true;
    worker_thread_ = std::thread(&PlayerCore::run_loop, this);
    return true;
}

void PlayerCore::shutdown() {
    if (!running_) return;
    send_command({PlayerCommand::QUIT});
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    running_ = false;
}

void PlayerCore::run_loop() {
    while (running_) {
        PlayerCommand cmd = cmd_queue_.pop();
        if (cmd.type == PlayerCommand::QUIT) break;
        process_command(cmd);
    }

    // Emit final state
    emit_event({PlayerEvent::STATE_CHANGED, PlaybackState::STOPPED});
}

void PlayerCore::process_command(const PlayerCommand& cmd) {
    switch (cmd.type) {
    case PlayerCommand::LOAD:
        // TODO
        break;
    case PlayerCommand::PLAY:
        state_ = PlaybackState::PLAYING;
        emit_event({PlayerEvent::STATE_CHANGED, state_});
        break;
    case PlayerCommand::PAUSE:
        state_ = PlaybackState::PAUSED;
        emit_event({PlayerEvent::STATE_CHANGED, state_});
        break;
    case PlayerCommand::STOP:
        state_ = PlaybackState::STOPPED;
        emit_event({PlayerEvent::STATE_CHANGED, state_});
        break;
    default:
        break;
    }
}

void PlayerCore::emit_event(PlayerEvent event) {
    if (event_cb_) {
        event_cb_(event);
    }
}

// ---- Factory ----

std::unique_ptr<Player> CreatePlayer() {
    return std::make_unique<PlayerCore>();
}

}  // namespace vsr
