#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vsr {

/// Track metadata for track selection (audio, subtitle, video).
struct TrackInfo {
    enum Type { AUDIO, SUBTITLE, VIDEO };
    Type type = AUDIO;
    int index = -1;                    // ffmpeg stream index
    bool external = false;             // external subtitle file
    bool active = false;               // currently selected

    // Audio fields
    std::string audio_codec;           // "aac", "opus", "flac" ...
    int channels = 0;
    int sample_rate = 0;

    // Subtitle fields
    std::string subtitle_codec;        // "ass", "srt", "subrip" ...

    // Common
    std::string language;              // "chi", "jpn", "eng" ...
    std::string title;                 // stream title or filename
};

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

/// Command sent from client to player core.
struct PlayerCommand {
    enum Type {
        // Media source
        LOAD_FILE,
        OPEN_STREAM,

        // Playback control
        PLAY,
        PAUSE,
        STOP,
        SEEK,

        // Window
        RESIZE,

        // Track management
        GET_TRACKS,
        SET_AUDIO_TRACK,
        SET_SUBTITLE_TRACK,
        ADD_SUBTITLE,
        REMOVE_SUBTITLE,

        // Runtime parameters
        SET_QUALITY,
        SET_SCALE,
        SET_VOLUME,
        SET_MUTE,
        SET_PLAYBACK_SPEED,
        SET_LOOP,

        // Debug
        CAPTURE_FRAME,

        // Lifecycle
        QUIT,
    };
    Type type = QUIT;
    std::string arg;
    int64_t seek_ms = 0;
    double volume = 1.0;
    double speed = 1.0;
    std::vector<std::pair<std::string, std::string>> options;
};

/// Event emitted from player core to client.
struct PlayerEvent {
    enum Type {
        STATE_CHANGED,
        POSITION_CHANGED,
        VIDEO_INFO,
        TRACKS_INFO,
        FRAME_INFO,
        ERROR,
        END_OF_FILE,
        FRAME_CAPTURED,
    };
    Type type = STATE_CHANGED;

    // STATE_CHANGED
    PlaybackState state = PlaybackState::STOPPED;

    // POSITION_CHANGED
    int64_t time_ms = 0;
    int64_t duration_ms = 0;

    // VIDEO_INFO / FRAME_INFO
    int in_width = 0, in_height = 0;
    int out_width = 0, out_height = 0;
    double fps = 0.0;
    int scale = 1;
    Quality quality = Quality::HIGH;
    bool hw_decoding = false;
    bool vsr_active = false;
    bool has_audio = false;
    bool seekable = true;

    // TRACKS_INFO
    std::vector<TrackInfo> tracks;

    // ERROR
    std::string error_msg;

    // FRAME_CAPTURED
    const uint8_t* capture_orig_data = nullptr;
    const uint8_t* capture_vsr_data = nullptr;
    int capture_orig_w = 0, capture_orig_h = 0;
    int capture_vsr_w = 0, capture_vsr_h = 0;
};

/// Callback type for player events.
using EventCallback = std::function<void(const PlayerEvent&)>;

/// Public API for the VSR player core.
///
/// All commands are enqueued via send_command() and processed
/// asynchronously on the worker thread. Events are dispatched
/// via the callback from the worker thread — the client must
/// marshal UI updates to its own thread.
class Player {
public:
    virtual ~Player() = default;

    /// Set the callback for player-to-client events.
    virtual void set_event_callback(EventCallback cb) = 0;

    /// Enqueue a command. Thread-safe, returns immediately.
    virtual void send_command(PlayerCommand cmd) = 0;

    /// Initialize the engine with a Wayland surface for Vulkan present.
    /// Creates VkInstance/Device/Surface on the calling thread, then hands
    /// off to the worker thread for all rendering operations.
    /// @param native_window   wl_surface from the Qt widget
    /// @param native_display  wl_display from the Qt platform
    /// @param use_vsr         Enable VSR AI super-resolution
    /// @param quality         Initial VSR quality level
    /// @param no_hwaccel      If true, disable NVDEC — force software decode
    virtual bool initialize(void* native_window, void* native_display,
                            bool use_vsr = true,
                            Quality quality = Quality::HIGH,
                            bool no_hwaccel = false) = 0;

    /// Shut down: stops playback, joins worker thread, releases all GPU
    /// resources.
    virtual void shutdown() = 0;
};

/// Factory function — creates a Player implementation.
std::unique_ptr<Player> CreatePlayer();

}  // namespace vsr
