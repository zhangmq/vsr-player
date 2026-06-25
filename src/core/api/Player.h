#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace vsr {

// ── Track metadata (unchanged) ─────────────────────────────────────

struct TrackInfo {
    enum Type { AUDIO, SUBTITLE, VIDEO };
    Type type = AUDIO;
    int index = -1;
    bool external = false;
    bool active = false;
    std::string audio_codec;
    int channels = 0;
    int sample_rate = 0;
    std::string subtitle_codec;
    std::string language;
    std::string title;
};

// ── Quality (unchanged) ────────────────────────────────────────────

enum class Quality { LOW, MEDIUM, HIGH, ULTRA };

// ── Rendering path ─────────────────────────────────────────────────

enum class Path { VSR, NOVSR };

// ── Playback state (unchanged) ─────────────────────────────────────

enum class PlaybackState { STOPPED, PLAYING, PAUSED };

// ── IVulkanContext — client-provided Vulkan handles ─────────────────

class IVulkanContext {
public:
    virtual ~IVulkanContext() = default;

    // ── Resource handles — core uses as factory, never modifies ──
    virtual void* vkInstance()       const = 0;
    virtual void* vkPhysicalDevice() const = 0;
    virtual void* vkDevice()         const = 0;
    virtual void* vkQueue()          const = 0;
    virtual int   vkQueueFamily()    const = 0;
    virtual void* vkCommandPool()    const = 0;
    virtual void* vkRenderPass()     const = 0;

    // ── Synchronization — affects client's shared queue/device ──
    virtual void waitIdle() const = 0;
    virtual void submitAndWait(void* commandBuffer) const = 0;
};

// ── Typed commands — zero field overloading ────────────────────────

struct CmdPlay  {};
struct CmdPause {};
struct CmdStop  {};
struct CmdQuit  {};

struct CmdLoadFile   { std::string path; };
struct CmdSeek       { int64_t position_ms; };
struct CmdResize     { int phys_w; int phys_h; };
struct CmdSetQuality { int level; };  // VFX quality value (1-4 upscale, 8-11 denoise)
struct CmdSetScale   { int scale; };          // 0=auto, 1-4=locked
struct CmdSetVolume  { double value; };       // 0.0 - 1.0
struct CmdSetMute    { bool muted; };
struct CmdSetHwaccel { bool enabled; };       // toggle NVDEC (applies on next LOAD_FILE)
struct CmdSetDenoiseQuality { int level; };   // -1=off, 8-11=low-ultra
struct CmdSetSpeed    { double speed; };         // playback speed multiplier (0.5, 0.75, 1.0, 2.0)
struct CmdCapture     {};  // request screenshot on next frame

using PlayerCommand = std::variant<
    CmdPlay, CmdPause, CmdStop, CmdQuit,
    CmdLoadFile, CmdSeek, CmdResize,
    CmdSetQuality, CmdSetScale, CmdSetVolume, CmdSetMute,
    CmdSetHwaccel, CmdSetSpeed, CmdSetDenoiseQuality,
    CmdCapture>;

// ── PlayerEvent (unchanged fields, cleaned up) ─────────────────────

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
        OPERATION_PENDING,
    };
    Type type = STATE_CHANGED;

    PlaybackState state = PlaybackState::STOPPED;
    int64_t time_ms = 0;
    int64_t duration_ms = 0;

    int in_width = 0, in_height = 0;
    int out_width = 0, out_height = 0;
    double fps = 0.0;
    int scale = 1;
    int quality = 3;
    bool hw_decoding = false;
    bool vsr_active = false;
    bool has_audio = false;
    bool seekable = true;

    std::vector<TrackInfo> tracks;
    std::string error_msg;

    const uint8_t* capture_orig_data = nullptr;
    const uint8_t* capture_vsr_data = nullptr;
    int capture_orig_w = 0, capture_orig_h = 0;
    int capture_vsr_w = 0, capture_vsr_h = 0;

    // OPERATION_PENDING fields (full target state)
    std::string pending_file;
    int pending_scale = -1;
    int pending_quality = -1;
    int pending_phys_w = 0;
    int pending_phys_h = 0;

    // FRAME_INFO fields (emitted every ~30 frames for OSD)
    int denoise = -1;
    double speed = 1.0;
    double render_fps = 0.0;
    int64_t frame_idx = 0;
    std::string gpu_name;
    int vram_used_mb = 0;
    int vram_total_mb = 0;
    int audio_sr = 0;
    int audio_ch = 0;
    std::string codec_name;
    std::string pix_fmt_name;
};

using EventCallback = std::function<void(const PlayerEvent&)>;

// ── Player interface ───────────────────────────────────────────────

class Player {
public:
    virtual ~Player() = default;

    /// Initialize the player. IVulkanContext must outlive the Player.
    /// SPIR-V pointers are borrowed (embedded in the binary, live forever).
    virtual bool initialize(
        IVulkanContext* vk,
        const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
        const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
        const uint32_t* vertSpv, size_t vertSpvLen,
        int quality = 3,
        bool no_hwaccel = false) = 0;

    virtual void send_command(PlayerCommand cmd) = 0;
    virtual void record_frame(void* cb, int w, int h) = 0;
    virtual void set_event_callback(EventCallback cb) = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<Player> CreatePlayer();

}  // namespace vsr
