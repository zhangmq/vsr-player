#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vsr {

/// Quality level for VSR processing (maps to NvVFX QualityLevel).
enum class Quality {
    LOW,
    MEDIUM,
    HIGH,
    ULTRA,
};

/// Playback state.
enum class PlaybackState {
    STOPPED,
    PLAYING,
    PAUSED,
};

/// Event emitted from player core to client.
struct PlayerEvent {
    enum Type {
        STATE_CHANGED,
        POSITION_CHANGED,  // time_ms updated
        FRAME_INFO,        // fps, resolution
        ERROR,
        END_OF_FILE,
    };
    Type type;
    PlaybackState state = PlaybackState::STOPPED;
    int64_t time_ms = 0;
    int64_t duration_ms = 0;
    double fps = 0.0;
    int in_width = 0;
    int in_height = 0;
    int out_width = 0;
    int out_height = 0;
    Quality quality = Quality::HIGH;
    std::string error_msg;
};

/// Command sent from client to player core.
struct PlayerCommand {
    enum Type {
        LOAD,
        PLAY,
        PAUSE,
        STOP,
        SEEK,
        SET_QUALITY,
        SET_VOLUME,
        SET_PLAYLIST,
        QUIT,
    };
    Type type;
    std::string arg;       // file path, quality string, etc.
    int64_t seek_ms = 0;
    double volume = 1.0;
    std::vector<std::string> playlist;
};

/// Callback type for player events.
using EventCallback = std::function<void(const PlayerEvent&)>;

/// Public API for the VSR player core.
///
/// All methods are thread-safe. Commands are enqueued and processed
/// asynchronously by worker threads. Events are dispatched via
/// the callback set by set_event_callback().
class Player {
public:
    virtual ~Player() = default;

    /// Set the callback for player-to-client events.
    virtual void set_event_callback(EventCallback cb) = 0;

    /// Enqueue a command. Returns immediately.
    virtual void send_command(PlayerCommand cmd) = 0;

    /// Initialize the player (Vulkan surface, threads).
    /// Must be called once before any commands.
    virtual bool initialize(void* vulkan_surface) = 0;

    /// Shut down, join all threads.
    virtual void shutdown() = 0;
};

/// Factory function — creates a Player implementation.
std::unique_ptr<Player> CreatePlayer();

}  // namespace vsr
