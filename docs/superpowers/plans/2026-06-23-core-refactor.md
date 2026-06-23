# Core/Client Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move all video playback pipeline logic from MainWindow into PlayerCore, making core a self-contained engine and client a thin Qt shell.

**Architecture:** PlayerCore owns its own worker thread, all Vulkan/CUDA resources, and the full demux→decode→VSR→render pipeline. Client sends commands via CommandQueue, receives events via callback (marshaled to Qt main thread via QMetaObject::invokeMethod).

**Tech Stack:** C++20, Qt6 (Widgets), FFmpeg, CUDA, Vulkan, PortAudio, NvVFX SDK, libpng

---

## File Change Matrix

| File | Status | Changes |
|------|--------|---------|
| `src/core/api/Player.h` | **Rewrite** | Full API: commands, events, TrackInfo |
| `src/core/PlayerCore.h` | **Rewrite** | Full class with all pipeline members |
| `src/core/PlayerCore.cpp` | **Rewrite** | 550+ lines: run_loop, process_one_frame, all cmd_* |
| `src/core/AudioOutput.h` | **Modify** | Add `write_pcm()`, change `open()` signature |
| `src/core/AudioOutput.cpp` | **Modify** | Implement PCM write path |
| `src/client/MainWindow.h` | **Rewrite** | Thin Qt shell, only UI + Player ptr |
| `src/client/MainWindow.cpp` | **Rewrite** | ~250 lines: init_player, on_player_event, save_screenshots |
| `src/client/VulkanWidget.h` | **Simplify** | Remove all Vulkan members, keep surface only |
| `src/client/VulkanWidget.cpp` | **Simplify** | ~25 lines: constructor only |
| `src/client/main.cpp` | **Modify** | New init order: show→init_player→open_file |
| `Makefile` | **Modify** | Update MainWindow deps (no more core headers) |

---

### Task 1: Extend Player.h — full API types

**Files:**
- Modify: `src/core/api/Player.h:1-97`

**Goal:** Define all command types, event types, TrackInfo, and data fields needed by the new API. Keep existing Player abstract class, add TrackInfo before it.

- [ ] **Step 1: Add TrackInfo struct**

Add before the existing `Quality` enum:

```cpp
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
```

- [ ] **Step 2: Extend PlayerCommand enum**

Replace the existing `PlayerCommand::Type` with the full set:

```cpp
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
```

- [ ] **Step 3: Extend PlayerEvent struct**

Add new fields to the existing `PlayerEvent`:

```cpp
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
```

- [ ] **Step 4: Update Player abstract class**

Adjust signatures — `initialize` now takes `use_vsr` and `quality`:

```cpp
class Player {
public:
    virtual ~Player() = default;
    virtual void set_event_callback(EventCallback cb) = 0;
    virtual void send_command(PlayerCommand cmd) = 0;
    virtual bool initialize(void* native_window, void* native_display,
                            bool use_vsr = true,
                            Quality quality = Quality::HIGH) = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<Player> CreatePlayer();
```

- [ ] **Step 5: Verify header compiles**

Run: `g++ -std=c++20 -Isrc/core -Isrc/core/api -fsyntax-only src/core/api/Player.h`
Expected: No errors.

- [ ] **Step 6: Commit**

```bash
git add src/core/api/Player.h
git commit -m "feat: extend Player API with track management, screenshot, and full command/event set"
```

---

### Task 2: Extend AudioOutput — PCM write interface

**Files:**
- Modify: `src/core/AudioOutput.h:26-28`
- Modify: `src/core/AudioOutput.cpp`

**Goal:** Add a `write_pcm()` method so Core can feed decoded PCM data instead of AudioOutput opening files itself. Keep backward-compatible — don't remove existing `open(const char*)` yet.

- [ ] **Step 1: Add new open() overload and write_pcm() declaration**

In `AudioOutput.h`, add after the existing `open(const char* file_path)`:

```cpp
/// Open audio output for PCM playback (no file — data fed via write_pcm).
/// @param sample_rate  Audio sample rate (e.g. 48000)
/// @param channels     Channel count (1 = mono, 2 = stereo)
bool open(int sample_rate, int channels);

/// Write interleaved float32 PCM samples to the ring buffer.
/// Thread-safe. Non-blocking return (data is dropped if buffer full).
/// @param data         Interleaved float32 PCM [-1.0, 1.0]
/// @param num_samples  Number of samples per channel (not frames!)
void write_pcm(const float* data, int num_samples);
```

- [ ] **Step 2: Implement open(int, int)**

In `AudioOutput.cpp`, add:

```cpp
bool AudioOutput::open(int sample_rate, int channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    has_audio_ = true;

    // Allocate ring buffer (0.5 seconds)
    ring_capacity_ = static_cast<int>(sample_rate * kBufferSec) * channels;
    ring_buf_.resize(ring_capacity_);
    ring_read_ = 0;
    ring_write_ = 0;
    ring_filled_ = 0;

    return true;
}
```

- [ ] **Step 3: Implement write_pcm()**

```cpp
void AudioOutput::write_pcm(const float* data, int num_samples) {
    if (!has_audio_ || !running_ || paused_) return;
    write_ring(data, num_samples);
}
```

Note: `write_ring` already exists — verifies it writes `num_samples` floats (not frames). If it takes frame count, adjust:

```cpp
int AudioOutput::write_ring(const float* data, int num_floats) {
    int avail = ring_capacity_ - ring_filled_.load();
    int to_write = std::min(num_floats, avail);
    if (to_write <= 0) return 0;

    int w = ring_write_.load();
    int first = std::min(to_write, ring_capacity_ - w);
    std::memcpy(ring_buf_.data() + w, data, first * sizeof(float));
    if (to_write > first)
        std::memcpy(ring_buf_.data(), data + first,
                    (to_write - first) * sizeof(float));

    ring_write_.store((w + to_write) % ring_capacity_);
    ring_filled_.fetch_add(to_write);
    return to_write;
}
```

- [ ] **Step 4: Verify AudioOutput compiles**

Run: `g++ -std=c++20 -Isrc/core -Isrc/core/api $(pkg-config --cflags portaudio-2.0) -fsyntax-only src/core/AudioOutput.h`
Expected: No errors.

- [ ] **Step 5: Commit**

```bash
git add src/core/AudioOutput.h src/core/AudioOutput.cpp
git commit -m "feat: add PCM write interface to AudioOutput for core-driven audio"
```

---

### Task 3: Rewrite PlayerCore.h — full class declaration

**Files:**
- Modify: `src/core/PlayerCore.h:1-55`

**Goal:** Declare the full PlayerCore class with all pipeline members, command handlers, and internal methods.

- [ ] **Step 1: Write the full header**

```cpp
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "api/Player.h"
#include "CommandQueue.h"

namespace vsr {

class ClockManager;
class Demuxer;
class Decoder;
class VSRProcessor;
class AudioOutput;
class VulkanRenderer;
class CUDAContext;
class NV12ToRGB;

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

    // ── Surface handles (set by initialize, used on worker) ──
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
    std::unique_ptr<ClockManager>  clock_;

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
```

- [ ] **Step 2: Verify header compiles**

Run: `g++ -std=c++20 -Isrc/core -Isrc/core/api -Isrc/core/utils -fsyntax-only src/core/PlayerCore.h`
Expected: No errors.

- [ ] **Step 3: Commit**

```bash
git add src/core/PlayerCore.h
git commit -m "feat: declare full PlayerCore class with pipeline ownership and command handlers"
```

---

### Task 4: Implement PlayerCore.cpp — full engine

**Files:**
- Modify: `src/core/PlayerCore.cpp:1-91`

**Goal:** Implement the full PlayerCore: initialize, run_loop, command dispatch, build_pipeline, process_one_frame, teardown_pipeline.

- [ ] **Step 1: Write includes and factory**

Replace entire file content:

```cpp
#include "PlayerCore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <cuda.h>

#include "AudioOutput.h"
#include "ClockManager.h"
#include "Decoder.h"
#include "Demuxer.h"
#include "VSRProcessor.h"
#include "utils/CUDAContext.h"
#include "utils/InteropTexture.h"
#include "utils/NV12ToRGB.h"
#include "utils/VulkanRenderer.h"

namespace vsr {

// ── Factory ──────────────────────────────────────────────────────────

std::unique_ptr<Player> CreatePlayer() {
    return std::make_unique<PlayerCore>();
}
```

- [ ] **Step 2: Write lifecycle (constructor, destructor, set_event_callback, send_command)**

```cpp
// ── Lifecycle ────────────────────────────────────────────────────────

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
```

- [ ] **Step 3: Write initialize()**

```cpp
bool PlayerCore::initialize(void* native_window, void* native_display,
                             bool use_vsr, Quality quality) {
    native_window_ = native_window;
    native_display_ = native_display;
    use_vsr_ = use_vsr;
    quality_ = quality;

    running_ = true;
    worker_thread_ = std::thread(&PlayerCore::run_loop, this);
    return true;
}
```

- [ ] **Step 4: Write shutdown()**

```cpp
void PlayerCore::shutdown() {
    if (!running_) return;
    send_command({PlayerCommand::QUIT});
    if (worker_thread_.joinable())
        worker_thread_.join();
    running_ = false;
}
```

- [ ] **Step 5: Write emit_event()**

```cpp
void PlayerCore::emit_event(PlayerEvent e) {
    if (event_cb_)
        event_cb_(e);
}
```

- [ ] **Step 6: Write run_loop()**

```cpp
// ── Main loop ────────────────────────────────────────────────────────

void PlayerCore::run_loop() {
    while (running_) {
        // Process all pending commands
        while (process_command_nonblock()) {}

        if (state_ == PlaybackState::PLAYING && demuxer_) {
            if (!process_one_frame()) {
                // EOF
                cmd_stop();
                emit_event({PlayerEvent::END_OF_FILE});
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    teardown_pipeline();

    emit_event({PlayerEvent::STATE_CHANGED, PlaybackState::STOPPED});
}
```

- [ ] **Step 7: Write process_command_nonblock() and dispatch**

```cpp
// ── Command dispatch ─────────────────────────────────────────────────

bool PlayerCore::process_command_nonblock() {
    PlayerCommand cmd;
    if (!cmd_queue_.try_pop(cmd)) return false;

    switch (cmd.type) {
    case PlayerCommand::LOAD_FILE:
        cmd_load_file(cmd.arg);
        break;
    case PlayerCommand::PLAY:
        cmd_play();
        break;
    case PlayerCommand::PAUSE:
        cmd_pause();
        break;
    case PlayerCommand::STOP:
        cmd_stop();
        break;
    case PlayerCommand::SEEK:
        cmd_seek(cmd.seek_ms);
        break;
    case PlayerCommand::RESIZE:
        cmd_resize((int)cmd.seek_ms, (int)cmd.volume);
        break;
    case PlayerCommand::SET_QUALITY:
        cmd_set_quality(cmd.arg == "low"    ? Quality::LOW :
                        cmd.arg == "medium" ? Quality::MEDIUM :
                        cmd.arg == "ultra"  ? Quality::ULTRA :
                                              Quality::HIGH);
        break;
    case PlayerCommand::SET_SCALE:
        cmd_set_scale((int)cmd.seek_ms);
        break;
    case PlayerCommand::SET_VOLUME:
        cmd_set_volume(cmd.volume);
        break;
    case PlayerCommand::SET_MUTE:
        cmd_set_mute(cmd.seek_ms != 0);
        break;
    case PlayerCommand::CAPTURE_FRAME:
        capture_pending_ = true;
        break;
    case PlayerCommand::QUIT:
        running_ = false;
        break;
    default:
        break;  // unimplemented commands are no-ops
    }
    return true;
}
```

- [ ] **Step 8: Write cmd_load_file() and build_pipeline()**

```cpp
// ── LOAD_FILE ────────────────────────────────────────────────────────

void PlayerCore::cmd_load_file(const std::string& path) {
    // Tear down previous pipeline
    teardown_pipeline();

    if (!build_pipeline(path)) {
        emit_event({PlayerEvent::ERROR, PlaybackState::STOPPED, 0, 0,
                    0, 0, 0, 0, 0, Quality::HIGH, false, false, false,
                    true, {}, "Failed to open file: " + path});
        return;
    }

    // Emit video info
    PlayerEvent info;
    info.type = PlayerEvent::VIDEO_INFO;
    info.in_width = video_w_;
    info.in_height = video_h_;
    info.fps = video_fps_;
    info.duration_ms = duration_ms_;
    info.hw_decoding = decoder_ ? decoder_->is_hardware() : false;
    info.has_audio = audio_ ? audio_->is_active() : false;
    info.seekable = seekable_;
    emit_event(info);

    // Apply pending resize if any
    if (pending_phys_w_ > 0 && pending_phys_h_ > 0) {
        cmd_resize(pending_phys_w_, pending_phys_h_);
    }
}

bool PlayerCore::build_pipeline(const std::string& path) {
    // ── Demuxer ──
    demuxer_ = std::make_unique<Demuxer>();
    if (!demuxer_->open(path)) {
        fprintf(stderr, "PlayerCore: demuxer open failed\n");
        return false;
    }

    video_w_ = demuxer_->video_width();
    video_h_ = demuxer_->video_height();
    video_fps_ = demuxer_->video_fps();
    duration_ms_ = demuxer_->duration_ms();
    seekable_ = duration_ms_ > 0;

    // ── Decoder ──
    decoder_ = std::make_unique<Decoder>();
    if (!decoder_->open(demuxer_->video_codecpar())) {
        fprintf(stderr, "PlayerCore: decoder open failed\n");
        return false;
    }

    // ── CUDA context ──
    cuda_ctx_ = std::make_unique<CUDAContext>();
    if (!cuda_ctx_->capture_current()) {
        if (!cuda_ctx_->init(0)) {
            fprintf(stderr, "PlayerCore: CUDA init failed\n");
            return false;
        }
    }
    cuda_ctx_->push();

    // ── CUDA stream ──
    cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);

    // ── NV12→RGB ──
    nv12_to_rgb_ = std::make_unique<NV12ToRGB>();
    if (!nv12_to_rgb_->compile()) {
        fprintf(stderr, "PlayerCore: NV12ToRGB compile failed\n");
        return false;
    }

    // ── GPU output buffer ──
    size_t rgb_bytes = NV12ToRGB::output_size(video_w_, video_h_);
    cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

    // ── Audio ──
    {
        audio_ = std::make_unique<AudioOutput>();
        int audio_idx = demuxer_->audio_stream_index();
        if (audio_idx >= 0) {
            audio_->open(demuxer_->audio_sample_rate(),
                         demuxer_->audio_channels());
            audio_->start();
        } else {
            audio_.reset();
        }
    }

    // ── VSR ──
    // Initial scale will be set by RESIZE command; default to 1x
    current_scale_ = 1;
    vsr_w_ = video_w_;
    vsr_h_ = video_h_;

    if (use_vsr_) {
        vsr_ = std::make_unique<VSRProcessor>();
        vsr_->set_stream(cuda_stream_);
        if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, quality_)) {
            fprintf(stderr, "PlayerCore: VSR init failed — NO-VSR mode\n");
            vsr_.reset();
        }
    }

    cuda_ctx_->pop();
    return true;
}
```

- [ ] **Step 9: Write teardown_pipeline()**

```cpp
void PlayerCore::teardown_pipeline() {
    if (cuda_ctx_)
        cuda_ctx_->push();

    // Release interop textures first (while Vulkan device alive)
    if (renderer_)
        renderer_->release();

    if (audio_) {
        audio_->stop();
        audio_.reset();
    }
    vsr_.reset();
    nv12_to_rgb_.reset();

    if (rgb_gpu_) {
        cuMemFree((CUdeviceptr)rgb_gpu_);
        rgb_gpu_ = nullptr;
    }
    if (cuda_stream_) {
        cuStreamDestroy((CUstream)cuda_stream_);
        cuda_stream_ = nullptr;
    }

    decoder_.reset();
    demuxer_.reset();

    if (cuda_ctx_) {
        cuda_ctx_->pop();
        cuda_ctx_.reset();
    }

    video_w_ = video_h_ = 0;
    video_fps_ = 0.0;
    frame_count_ = 0;
}
```

- [ ] **Step 10: Write play/pause/stop/seek/quality/scale/mute handlers**

```cpp
// ── Simple command handlers ──────────────────────────────────────────

void PlayerCore::cmd_play() {
    if (!demuxer_) return;
    state_ = PlaybackState::PLAYING;
    if (audio_) audio_->resume();
    emit_event({PlayerEvent::STATE_CHANGED, state_});
}

void PlayerCore::cmd_pause() {
    state_ = PlaybackState::PAUSED;
    if (audio_) audio_->pause();
    emit_event({PlayerEvent::STATE_CHANGED, state_});
}

void PlayerCore::cmd_stop() {
    state_ = PlaybackState::STOPPED;
    teardown_pipeline();
    emit_event({PlayerEvent::STATE_CHANGED, state_});
}

void PlayerCore::cmd_seek(int64_t ms) {
    if (!demuxer_ || !seekable_) return;
    // Flush decoder, seek demuxer
    if (decoder_) decoder_->flush();
    // TODO: av_seek_frame on demuxer_->fmt_ctx_
    if (audio_) audio_->seek(ms / 1000.0);
    frame_count_ = (int64_t)((ms / 1000.0) * video_fps_);
}

void PlayerCore::cmd_set_quality(Quality q) {
    quality_ = q;
    if (vsr_ && vsr_->is_ready())
        vsr_->reconfigure(vsr_w_, vsr_h_, q);
}

void PlayerCore::cmd_set_scale(int s) {
    if (s < 1 || s > 4 || s == current_scale_) return;
    current_scale_ = s;
    vsr_w_ = video_w_ * s;
    vsr_h_ = video_h_ * s;
    reconfigure_vsr();
}

void PlayerCore::cmd_set_volume(double vol) {
    // AudioOutput volume control — placeholder
    (void)vol;
}

void PlayerCore::cmd_set_mute(bool mute) {
    // AudioOutput mute control — placeholder
    (void)mute;
}
```

- [ ] **Step 11: Write cmd_resize()**

```cpp
void PlayerCore::cmd_resize(int phys_w, int phys_h) {
    if (phys_w <= 0 || phys_h <= 0) return;

    // Defer if pipeline not ready yet
    if (!demuxer_) {
        pending_phys_w_ = phys_w;
        pending_phys_h_ = phys_h;
        return;
    }
    pending_phys_w_ = pending_phys_h_ = 0;

    cuda_ctx_->push();

    // Compute adaptive scale
    int new_scale = 1;
    if (use_vsr_ && video_w_ > 0 && video_h_ > 0) {
        int sw = (phys_w + video_w_ - 1) / video_w_;
        int sh = (phys_h + video_h_ - 1) / video_h_;
        new_scale = std::clamp(std::min(sw, sh), 1, 4);
    }

    // Init renderer if first time
    if (!renderer_) {
        renderer_ = std::make_unique<VulkanRenderer>();
        if (!renderer_->init(native_window_, native_display_)) {
            fprintf(stderr, "PlayerCore: VulkanRenderer init failed\n");
            cuda_ctx_->pop();
            return;
        }
    }

    // Resize swapchain
    renderer_->resize(phys_w, phys_h);

    // Init or recreate pipelines if scale changed
    if (new_scale != current_scale_) {
        current_scale_ = new_scale;
        vsr_w_ = video_w_ * current_scale_;
        vsr_h_ = video_h_ * current_scale_;
        reconfigure_vsr();
    }

    // Init pipelines (first time or after resize)
    // SPIR-V: we need the shader data. For now, use saved pointers
    // passed via a separate init_pipelines call or stored in renderer.
    // See Task 8 (integration) for SPIR-V plumbing.
    if (!renderer_->is_ready() || new_scale != current_scale_) {
        renderer_->init_pipelines_with_saved_spv(
            video_w_, video_h_, current_scale_, phys_w, phys_h);
    }

    cuda_ctx_->pop();
}
```

Note: The SPIR-V shader data issue needs resolving — see Task 8 integration note.

- [ ] **Step 12: Write reconfigure_vsr()**

```cpp
void PlayerCore::reconfigure_vsr() {
    if (!use_vsr_) {
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
        return;
    }
    if (vsr_ && vsr_->is_ready()) {
        vsr_->reconfigure(vsr_w_, vsr_h_, quality_);
    } else if (!vsr_ && use_vsr_) {
        vsr_ = std::make_unique<VSRProcessor>();
        vsr_->set_stream(cuda_stream_);
        if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, quality_)) {
            fprintf(stderr, "PlayerCore: VSR init failed\n");
            vsr_.reset();
        }
    }
}
```

- [ ] **Step 13: Write process_one_frame() — the core decode+VSR+render loop**

Port the existing MainWindow::on_timer_tick() logic, adapted to run on worker thread:

```cpp
bool PlayerCore::process_one_frame() {
    cuda_ctx_->push();

    // 1. Read packet
    AVPacket* pkt = demuxer_->read_packet();
    if (!pkt) {
        cuda_ctx_->pop();
        return false;  // EOF
    }

    // Audio packets: decode and feed to audio sink
    if (pkt->stream_index == demuxer_->audio_stream_index() && audio_) {
        // Software decode audio packet → PCM → write_pcm
        // (simplified: decode happens in AudioOutput internally for now)
        av_packet_free(&pkt);
        cuda_ctx_->pop();
        return true;
    }

    if (pkt->stream_index != demuxer_->video_stream_index()) {
        av_packet_free(&pkt);
        cuda_ctx_->pop();
        return true;
    }

    // 2. Decode video
    decoder_->send_packet(pkt->data, pkt->size, pkt->pts);
    av_packet_free(&pkt);

    AVFrame* hw_frame = decoder_->receive_frame();
    if (!hw_frame) {
        cuda_ctx_->pop();
        return true;
    }

    // 3. Format check
    bool is_hw = (hw_frame->format == AV_PIX_FMT_CUDA);
    if (!is_hw && hw_frame->format != AV_PIX_FMT_NV12 &&
        hw_frame->format != AV_PIX_FMT_YUV420P) {
        decoder_->release_frame(hw_frame);
        cuda_ctx_->pop();
        return true;
    }

    // 4. YUV420P → NV12 interleave (if SW decode with planar output)
    bool is_yuv420p = (hw_frame->format == AV_PIX_FMT_YUV420P);
    std::vector<uint8_t> uv_interleaved;
    uint8_t* y_plane  = hw_frame->data[0];
    uint8_t* uv_plane = hw_frame->data[1];
    int y_pitch  = hw_frame->linesize[0];
    int uv_pitch = hw_frame->linesize[1];

    if (is_yuv420p) {
        uint8_t* u_plane = hw_frame->data[1];
        uint8_t* v_plane = hw_frame->data[2];
        int u_pitch = hw_frame->linesize[1];
        int v_pitch = hw_frame->linesize[2];
        int uv_w = video_w_ / 2;
        int uv_h = video_h_ / 2;
        int nv12_uv_pitch = video_w_;
        uv_interleaved.resize((size_t)nv12_uv_pitch * uv_h);
        for (int row = 0; row < uv_h; row++) {
            for (int col = 0; col < uv_w; col++) {
                uv_interleaved[row * nv12_uv_pitch + col * 2] =
                    u_plane[row * u_pitch + col];
                uv_interleaved[row * nv12_uv_pitch + col * 2 + 1] =
                    v_plane[row * v_pitch + col];
            }
        }
        uv_plane = uv_interleaved.data();
        uv_pitch = nv12_uv_pitch;
    }

    // 5. NV12→RGB GPU kernel
    {
        // For SW decode: H2D first
        CUdeviceptr tmp_y = 0, tmp_uv = 0;
        if (!is_hw) {
            size_t y_sz  = (size_t)y_pitch * video_h_;
            size_t uv_sz = (size_t)uv_pitch * (video_h_ / 2);
            cuMemAlloc(&tmp_y, y_sz);
            cuMemAlloc(&tmp_uv, uv_sz);
            cuMemcpyHtoDAsync(tmp_y,  y_plane,  y_sz,  (CUstream)cuda_stream_);
            cuMemcpyHtoDAsync(tmp_uv, uv_plane, uv_sz, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
        }

        uint8_t* gpu_y  = is_hw ? y_plane  : (uint8_t*)tmp_y;
        uint8_t* gpu_uv = is_hw ? uv_plane : (uint8_t*)tmp_uv;

        nv12_to_rgb_->convert(gpu_y, y_pitch, gpu_uv, uv_pitch,
                               video_w_, video_h_,
                               rgb_gpu_, cuda_stream_);

        if (!is_hw) { cuMemFree(tmp_y); cuMemFree(tmp_uv); }
    }

    // 6. VSR
    void* vsr_out_ptr = nullptr;
    int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
    if (vsr_ && vsr_->is_ready()) {
        vsr_->process(rgb_gpu_, &vsr_out_ptr, &vsr_out_w, &vsr_out_h,
                      &vsr_out_pitch);
    }

    // 7. D2D copy → InteropTexture + Vulkan render
    if (renderer_ && renderer_->is_ready()) {
        if (vsr_out_ptr) {
            // VSR path
            auto& rgbaTex = renderer_->rgbaInterop();
            size_t row_bytes = (size_t)vsr_out_w * 4;
            CUDA_MEMCPY2D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice     = (CUdeviceptr)vsr_out_ptr;
            copy.srcPitch      = (size_t)vsr_out_pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice     = rgbaTex.cudaPtr();
            copy.dstPitch      = rgbaTex.cudaPitch();
            copy.WidthInBytes  = row_bytes;
            copy.Height        = (size_t)vsr_out_h;
            cuMemcpy2DAsync(&copy, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
            vsr_w_ = vsr_out_w;
            vsr_h_ = vsr_out_h;
        } else {
            // NO-VSR path
            auto& yTex  = renderer_->yInterop();
            auto& uvTex = renderer_->uvInterop();
            CUDA_MEMCPY2D copyY = {};
            copyY.srcPitch      = (size_t)y_pitch;
            copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyY.dstDevice     = yTex.cudaPtr();
            copyY.dstPitch      = yTex.cudaPitch();
            copyY.WidthInBytes  = (size_t)video_w_;
            copyY.Height        = (size_t)video_h_;
            if (is_hw) {
                copyY.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                copyY.srcDevice     = (CUdeviceptr)y_plane;
            } else {
                copyY.srcMemoryType = CU_MEMORYTYPE_HOST;
                copyY.srcHost       = y_plane;
            }
            cuMemcpy2DAsync(&copyY, (CUstream)cuda_stream_);

            CUDA_MEMCPY2D copyUV = {};
            copyUV.srcPitch      = (size_t)uv_pitch;
            copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyUV.dstDevice     = uvTex.cudaPtr();
            copyUV.dstPitch      = uvTex.cudaPitch();
            copyUV.WidthInBytes  = (size_t)video_w_;
            copyUV.Height        = (size_t)(video_h_ / 2);
            if (is_hw) {
                copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                copyUV.srcDevice     = (CUdeviceptr)uv_plane;
            } else {
                copyUV.srcMemoryType = CU_MEMORYTYPE_HOST;
                copyUV.srcHost       = uv_plane;
            }
            cuMemcpy2DAsync(&copyUV, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
            vsr_w_ = video_w_;
            vsr_h_ = video_h_;
        }

        renderer_->render_frame(vsr_out_ptr ? Path::VSR : Path::NOVSR);
    }

    // 8. Screenshot capture
    if (capture_pending_) {
        capture_pending_ = false;
        // Original frame: DtoH rgb_gpu_
        int npixels = video_w_ * video_h_;
        size_t plane_bytes = (size_t)npixels * sizeof(float);
        capture_orig_buf_.resize((size_t)npixels * 3);
        {
            std::vector<float> rgb_cpu(npixels * 3);
            cuMemcpyDtoH(rgb_cpu.data(), (CUdeviceptr)rgb_gpu_, plane_bytes * 3);
            float* rp = rgb_cpu.data();
            float* gp = rgb_cpu.data() + npixels;
            float* bp = rgb_cpu.data() + npixels * 2;
            for (int i = 0; i < npixels; i++) {
                capture_orig_buf_[i * 3 + 0] = (uint8_t)std::clamp((int)(rp[i] * 255.0f), 0, 255);
                capture_orig_buf_[i * 3 + 1] = (uint8_t)std::clamp((int)(gp[i] * 255.0f), 0, 255);
                capture_orig_buf_[i * 3 + 2] = (uint8_t)std::clamp((int)(bp[i] * 255.0f), 0, 255);
            }
        }
        // VSR frame: DtoH vsr_out
        if (vsr_out_ptr && vsr_out_w > 0 && vsr_out_h > 0) {
            int out_pitch = vsr_out_pitch > 0 ? vsr_out_pitch : vsr_out_w * 4;
            size_t row_bytes = (size_t)vsr_out_w * 4;
            std::vector<uint8_t> rgba_cpu((size_t)out_pitch * vsr_out_h);
            CUDA_MEMCPY2D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice     = (CUdeviceptr)vsr_out_ptr;
            copy.srcPitch      = (size_t)out_pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost       = rgba_cpu.data();
            copy.dstPitch      = row_bytes;
            copy.WidthInBytes  = row_bytes;
            copy.Height        = (size_t)vsr_out_h;
            cuMemcpy2D(&copy);

            capture_vsr_buf_.resize((size_t)vsr_out_w * vsr_out_h * 3);
            for (int row = 0; row < vsr_out_h; row++) {
                uint8_t* src = rgba_cpu.data() + (size_t)row * row_bytes;
                uint8_t* dst = capture_vsr_buf_.data() + (size_t)row * vsr_out_w * 3;
                for (int col = 0; col < vsr_out_w; col++) {
                    dst[col * 3 + 0] = src[col * 4 + 0];
                    dst[col * 3 + 1] = src[col * 4 + 1];
                    dst[col * 3 + 2] = src[col * 4 + 2];
                }
            }
        }

        PlayerEvent ce;
        ce.type = PlayerEvent::FRAME_CAPTURED;
        ce.capture_orig_data = capture_orig_buf_.data();
        ce.capture_orig_w = video_w_;
        ce.capture_orig_h = video_h_;
        if (!capture_vsr_buf_.empty()) {
            ce.capture_vsr_data = capture_vsr_buf_.data();
            ce.capture_vsr_w = vsr_out_w;
            ce.capture_vsr_h = vsr_out_h;
        }
        emit_event(ce);
    }

    // 9. A/V sync
    if (audio_ && audio_->is_active()) {
        frame_count_++;
        double pts_sec = frame_count_ / video_fps_;
        double clock = audio_->clock_sec();
        double delay = pts_sec - clock;
        if (delay > 0.002) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(
                    std::min(delay * 1000.0, 50.0))));
        }
    } else {
        frame_count_++;
    }

    // 10. Periodic position update
    int64_t now_ms = (int64_t)(frame_count_ / video_fps_ * 1000.0);
    if (now_ms - last_position_emit_ms_ >= 200) {
        last_position_emit_ms_ = now_ms;
        PlayerEvent pe;
        pe.type = PlayerEvent::POSITION_CHANGED;
        pe.time_ms = now_ms;
        pe.duration_ms = duration_ms_;
        emit_event(pe);
    }

    // 11. Frame info event
    {
        PlayerEvent fi;
        fi.type = PlayerEvent::FRAME_INFO;
        fi.in_width = video_w_;
        fi.in_height = video_h_;
        fi.out_width = vsr_w_;
        fi.out_height = vsr_h_;
        fi.fps = video_fps_;
        fi.scale = current_scale_;
        fi.quality = quality_;
        fi.hw_decoding = decoder_->is_hardware();
        fi.vsr_active = (vsr_ && vsr_->is_ready());
        fi.has_audio = (audio_ && audio_->is_active());
        emit_event(fi);
    }

    decoder_->release_frame(hw_frame);
    cuda_ctx_->pop();
    return true;
}
```

- [ ] **Step 14: Verify compilation of PlayerCore.cpp**

Note: Full link will fail until remaining tasks are done (VulkanRenderer changes, MainWindow).

Run: `g++ -std=c++20 -Wall -Wextra -fPIC -O2 -DNDEBUG -Wno-missing-field-initializers $(pkg-config --cflags Qt6Widgets vulkan libavcodec libavformat libavutil libswscale wayland-client portaudio-2.0) -Ithird_party/cuda/include -Ithird_party/nvvfx/include -Isrc/core -Isrc/core/api -Isrc/core/utils -Ibuild/shaders -c src/core/PlayerCore.cpp -o build/src/core/PlayerCore.o`
Expected: Compiles to object file (may have warnings about unimplemented methods).

- [ ] **Step 15: Commit**

```bash
git add src/core/PlayerCore.cpp
git commit -m "feat: implement full PlayerCore engine with decode/VSR/render loop"
```

---

### Task 5: Simplify VulkanWidget — surface-only carrier

**Files:**
- Modify: `src/client/VulkanWidget.h:1-50`
- Modify: `src/client/VulkanWidget.cpp:1-92`

**Goal:** Strip VulkanWidget to a bare minimum QWidget that only provides a Wayland surface. Remove all Vulkan references.

- [ ] **Step 1: Rewrite VulkanWidget.h**

```cpp
#pragma once

#include <QWidget>

namespace vsr {

/// Minimal QWidget that provides a Wayland surface (wl_surface) for
/// the Core to render into via Vulkan. No Vulkan logic lives here —
/// all rendering is handled by PlayerCore on its worker thread.
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);

    QPaintEngine* paintEngine() const override { return nullptr; }
};

}  // namespace vsr
```

- [ ] **Step 2: Rewrite VulkanWidget.cpp**

```cpp
#include "VulkanWidget.h"

#include <cstdio>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

}  // namespace vsr
```

- [ ] **Step 3: Verify compiles**

Run: `g++ -std=c++20 $(pkg-config --cflags Qt6Widgets) -Isrc/client -Isrc/core/api -c src/client/VulkanWidget.cpp -o build/src/client/VulkanWidget.o`
Expected: Compiles.

- [ ] **Step 4: Commit**

```bash
git add src/client/VulkanWidget.h src/client/VulkanWidget.cpp
git commit -m "refactor: simplify VulkanWidget to surface-only Wayland carrier"
```

---

### Task 6: Rewrite MainWindow as thin client

**Files:**
- Modify: `src/client/MainWindow.h:1-104`
- Modify: `src/client/MainWindow.cpp:1-708`

**Goal:** MainWindow becomes a pure Qt shell — UI controls + Player proxy + screenshot PNG output.

- [ ] **Step 1: Rewrite MainWindow.h**

```cpp
#pragma once

#include <QKeyEvent>
#include <QMainWindow>
#include <memory>
#include <string>

#include "api/Player.h"

class QPushButton;
class QLabel;

namespace vsr {

class VulkanWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void init_player(bool use_vsr, Quality quality);
    void open_file(const QString& path);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void on_player_event(const PlayerEvent& e);
    void send_resize();

    // UI
    VulkanWidget* vulkan_widget_ = nullptr;
    QWidget*      overlay_ = nullptr;
    QPushButton*  play_btn_ = nullptr;
    QLabel*       status_label_ = nullptr;

    // Player engine
    std::unique_ptr<Player> player_;
    bool player_initialized_ = false;

    // Screenshot
    int screenshot_counter_ = 0;
    std::string screenshot_dir_ = "./screenshots";
    void save_screenshots(const PlayerEvent& e);
    static void save_png(const std::string& path, const uint8_t* rgb,
                         int w, int h);
};

}  // namespace vsr
```

- [ ] **Step 2: Write MainWindow.cpp — constructor, setup_ui, destructor**

```cpp
#include "MainWindow.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <png.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QtGui/qpa/qplatformnativeinterface.h>

#include "VulkanWidget.h"

namespace vsr {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("VSR Player");
    setMinimumSize(320, 180);

    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    vulkan_widget_ = new VulkanWidget(central);

    // Overlay control bar
    overlay_ = new QWidget(central);
    overlay_->setStyleSheet(
        "QWidget {"
        "  background: rgba(0, 0, 0, 0.55);"
        "  border-radius: 6px;"
        "}"
    );
    overlay_->setFixedHeight(48);

    auto* bar = new QHBoxLayout(overlay_);
    bar->setContentsMargins(12, 4, 12, 4);
    bar->setSpacing(10);

    play_btn_ = new QPushButton("▶ Play");
    play_btn_->setStyleSheet(
        "QPushButton {"
        "  background: rgba(255,255,255,0.15); color: white;"
        "  border: none; border-radius: 4px; padding: 6px 16px;"
        "  font-size: 13px;"
        "}"
        "QPushButton:hover { background: rgba(255,255,255,0.25); }"
    );

    status_label_ = new QLabel("No file loaded");
    status_label_->setStyleSheet("color: rgba(255,255,255,0.85); font-size: 13px;");

    bar->addWidget(play_btn_);
    bar->addWidget(status_label_, 1);

    connect(play_btn_, &QPushButton::clicked, this, [this]() {
        if (!player_initialized_) return;
        // Toggle play/pause
        static bool is_playing = false;
        is_playing = !is_playing;
        play_btn_->setText(is_playing ? "⏸ Pause" : "▶ Play");
        if (is_playing)
            player_->send_command({PlayerCommand::PLAY});
        else
            player_->send_command({PlayerCommand::PAUSE});
    });
}

MainWindow::~MainWindow() {
    if (player_)
        player_->shutdown();
}
```

- [ ] **Step 3: Write init_player()**

```cpp
void MainWindow::init_player(bool use_vsr, Quality quality) {
    auto* native = QGuiApplication::platformNativeInterface();
    void* display = native->nativeResourceForIntegration("wl_display");
    void* window  = reinterpret_cast<void*>(vulkan_widget_->winId());

    if (!display || !window) {
        status_label_->setText("Wayland display/surface not available");
        return;
    }

    player_ = CreatePlayer();
    player_->set_event_callback([this](const PlayerEvent& e) {
        // Worker thread → Qt main thread
        auto copy = std::make_shared<PlayerEvent>(e);
        QMetaObject::invokeMethod(this, [this, copy] {
            on_player_event(*copy);
        }, Qt::QueuedConnection);
    });

    if (player_->initialize(window, display, use_vsr, quality))
        player_initialized_ = true;
    else
        status_label_->setText("Player init failed");
}
```

- [ ] **Step 4: Write open_file()**

```cpp
void MainWindow::open_file(const QString& path) {
    if (!player_initialized_) return;
    status_label_->setText("Loading...");
    play_btn_->setText("⏸ Pause");  // will auto-play after load
    player_->send_command({PlayerCommand::LOAD_FILE, path.toStdString()});
}
```

- [ ] **Step 5: Write on_player_event()**

```cpp
void MainWindow::on_player_event(const PlayerEvent& e) {
    switch (e.type) {
    case PlayerEvent::VIDEO_INFO: {
        // Resize window to match video dimensions
        if (e.in_width > 0 && e.in_height > 0) {
            // Clamp to 90% of screen size
            auto* screen = QGuiApplication::primaryScreen();
            if (screen) {
                QSize avail = screen->availableGeometry().size();
                int max_w = avail.width() * 9 / 10;
                int max_h = avail.height() * 9 / 10;
                float scale = std::min((float)max_w / e.in_width,
                                       (float)max_h / e.in_height);
                resize((int)(e.in_width * scale), (int)(e.in_height * scale));
            } else {
                resize(e.in_width, e.in_height);
            }
            // resizeEvent will fire → send_resize()
        }
        setWindowTitle(QString("VSR Player — %1 fps").arg(e.fps, 0, 'f', 1));
        player_->send_command({PlayerCommand::PLAY});
        break;
    }
    case PlayerEvent::STATE_CHANGED: {
        bool playing = (e.state == PlaybackState::PLAYING);
        play_btn_->setText(playing ? "⏸ Pause" : "▶ Play");
        break;
    }
    case PlayerEvent::FRAME_INFO: {
        const char* qstr = e.quality == Quality::LOW    ? "LOW" :
                           e.quality == Quality::MEDIUM ? "MEDIUM" :
                           e.quality == Quality::HIGH   ? "HIGH" : "ULTRA";
        const char* mode = e.vsr_active ? (e.scale > 1 ? "UPSCALE" : "DENOISE") : "NO-VSR";
        status_label_->setText(QString("%1×%2 → %3×%4 x%5 [%6-%7] %8 %9")
            .arg(e.in_width).arg(e.in_height)
            .arg(e.out_width).arg(e.out_height)
            .arg(e.scale).arg(mode).arg(qstr)
            .arg(e.hw_decoding ? "[NVDEC]" : "[SW]")
            .arg(e.has_audio ? "[AUDIO]" : ""));
        break;
    }
    case PlayerEvent::ERROR:
        status_label_->setText(QString("Error: %1").arg(QString::fromStdString(e.error_msg)));
        break;
    case PlayerEvent::END_OF_FILE:
        status_label_->setText("End of file");
        player_->send_command({PlayerCommand::PAUSE});
        break;
    case PlayerEvent::FRAME_CAPTURED:
        save_screenshots(e);
        break;
    default:
        break;
    }
}
```

- [ ] **Step 6: Write resizeEvent, send_resize, keyPressEvent**

```cpp
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    const int w = centralWidget()->width();
    const int h = centralWidget()->height();
    vulkan_widget_->setGeometry(0, 0, w, h);
    overlay_->setGeometry(12, h - 60, w - 24, 48);

    if (player_initialized_)
        send_resize();
}

void MainWindow::send_resize() {
    qreal dpr = vulkan_widget_->devicePixelRatio();
    int phys_w = (int)(vulkan_widget_->width() * dpr);
    int phys_h = (int)(vulkan_widget_->height() * dpr);
    if (phys_w > 0 && phys_h > 0)
        player_->send_command({PlayerCommand::RESIZE, "", phys_w, phys_h, 0});
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_S && !event->isAutoRepeat()) {
        player_->send_command({PlayerCommand::CAPTURE_FRAME});
        status_label_->setText("Screenshot queued...");
    }
    QMainWindow::keyPressEvent(event);
}
```

- [ ] **Step 7: Write screenshot helpers (save_screenshots, save_png)**

```cpp
void MainWindow::save_screenshots(const PlayerEvent& e) {
    mkdir(screenshot_dir_.c_str(), 0755);
    int n = screenshot_counter_++;
    char path[256];

    if (e.capture_orig_data && e.capture_orig_w > 0) {
        snprintf(path, sizeof(path), "%s/%05d_orig.png",
                 screenshot_dir_.c_str(), n);
        save_png(path, e.capture_orig_data,
                 e.capture_orig_w, e.capture_orig_h);
    }
    if (e.capture_vsr_data && e.capture_vsr_w > 0) {
        snprintf(path, sizeof(path), "%s/%05d_vsr.png",
                 screenshot_dir_.c_str(), n);
        save_png(path, e.capture_vsr_data,
                 e.capture_vsr_w, e.capture_vsr_h);
    }

    status_label_->setText(QString("Screenshot %1 saved").arg(n));
}

void MainWindow::save_png(const std::string& path, const uint8_t* rgb,
                           int w, int h) {
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { fprintf(stderr, "Screenshot: fopen %s failed\n", path.c_str()); return; }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); return; }

    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (int y = 0; y < h; y++)
        png_write_row(png, rgb + (size_t)y * w * 3);

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    printf("Screenshot: saved %s (%dx%d)\n", path.c_str(), w, h);
}
```

- [ ] **Step 8: Verify compiles**

Run: `g++ -std=c++20 $(pkg-config --cflags Qt6Widgets libpng) -Isrc/client -Isrc/core/api -c src/client/MainWindow.cpp -o build/src/client/MainWindow.o`
Expected: Compiles.

- [ ] **Step 9: Commit**

```bash
git add src/client/MainWindow.h src/client/MainWindow.cpp
git commit -m "refactor: rewrite MainWindow as thin Qt shell using Player API"
```

---

### Task 7: Update main.cpp — new initialization order

**Files:**
- Modify: `src/client/main.cpp`

**Goal:** Change init order: show window first (→ wl_surface), then init_player, then open_file.

- [ ] **Step 1: Read current main.cpp**

Read: `src/client/main.cpp`

- [ ] **Step 2: Adjust init order**

The key change: `window.init_player()` must be called AFTER `window.show()` (so wl_surface exists). Then `window.open_file()`.

Current pattern (approx):
```cpp
MainWindow window(use_vsr, quality);
window.set_no_hwaccel(no_hwaccel);
window.show();
if (!file_path.isEmpty())
    window.open_file(file_path);
```

New pattern:
```cpp
MainWindow window;
window.show();                          // ← first: wl_surface created
window.init_player(use_vsr, quality);   // ← second: core takes surface
if (!file_path.isEmpty())
    window.open_file(file_path);        // ← third: load
```

Also remove `set_no_hwaccel(false)` — the `--no-hwaccel` flag isn't wired up in the new API yet (decoder is always created inside core). If needed, add a `force_sw` field to `initialize()` later.

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp
git commit -m "fix: reorder init sequence — show window before player init"
```

---

### Task 8: Integration — SPIR-V plumbing, Makefile, VulkanRenderer adapter

**Files:**
- Modify: `src/core/utils/VulkanRenderer.h`
- Modify: `src/core/utils/VulkanRenderer.cpp`
- Modify: `Makefile`

**Goal:** Add a method to VulkanRenderer that stores SPIR-V and initializes pipelines without requiring client to pass shader data each time. Update Makefile for new dependencies.

- [ ] **Step 1: Add SPIR-V storage to VulkanRenderer**

In `VulkanRenderer.h`, add:

```cpp
/// Store SPIR-V data for pipeline recreation (called once after init).
void set_shader_data(const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                     const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                     const uint32_t* vertSpv, size_t vertSpvLen);

/// Init or recreate pipelines using previously stored SPIR-V.
bool init_pipelines_with_saved_spv(int videoW, int videoH, int scale,
                                    int widgetW, int widgetH);
```

Implementation in `VulkanRenderer.cpp` — `set_shader_data()` saves to member fields (already exist). `init_pipelines_with_saved_spv()` calls the existing `init_pipelines()` with the saved SPIR-V pointers.

- [ ] **Step 2: PlayerCore uses set_shader_data()**

In `cmd_resize()`, the first time the renderer is created, call:

```cpp
renderer_->set_shader_data(
    reinterpret_cast<const uint32_t*>(video_frag_spv), video_frag_spv_len,
    reinterpret_cast<const uint32_t*>(nv12_frag_spv), nv12_frag_spv_len,
    reinterpret_cast<const uint32_t*>(video_vert_spv), video_vert_spv_len);
```

PlayerCore.cpp includes generated SPIR-V headers (add `#include "video_vert_spv.h"` etc.).

- [ ] **Step 3: Update Makefile**

- Add `$(BUILD_DIR)/src/core/PlayerCore.o` to `CORE_OBJS`
- MainWindow.o no longer depends on `$(SHADERS)` (that dependency moves to PlayerCore.o)
- Add PlayerCore.o dep on SPIR-V headers: `$(BUILD_DIR)/src/core/PlayerCore.o: $(SHADERS)`
- Update MainWindow.o CXXFLAGS: remove `-Isrc/core -Isrc/core/utils -I$(CUDA_INC) -Ithird_party/nvvfx/include`
- MainWindow.o only needs: `-Isrc/core/api -Isrc/client $(pkg-config --cflags Qt6Widgets libpng)`
- Remove obsolete MOC files if VulkanWidget no longer has Q_OBJECT (it still does)

- [ ] **Step 4: Full build**

Run: `make clean && make -j$(nproc)`
Expected: Successful link.

- [ ] **Step 5: Commit**

```bash
git add src/core/utils/VulkanRenderer.h src/core/utils/VulkanRenderer.cpp Makefile
git commit -m "feat: add SPIR-V storage to VulkanRenderer for core-driven pipeline init"
```

---

### Task 9: Integration test — build, run, fix

**Files:** Various (fixes)

**Goal:** Build the full binary, test playback, fix any issues.

- [ ] **Step 1: Build**

Run: `make clean && make -j$(nproc)`
Expected: Binary at `build/vsr-player`.

- [ ] **Step 2: Test basic playback (HW decode + VSR)**

Run: `./build/vsr-player input/catlove_720p.webm`
Expected: Window opens, video plays with VSR upscale, audio plays, status bar shows info.

- [ ] **Step 3: Test --no-vsr path**

Run: `./build/vsr-player --no-vsr input/catlove_720p.webm`
Expected: Video plays at native resolution via NV12 interop.

- [ ] **Step 4: Test --no-hwaccel path**

Run: `./build/vsr-player --no-hwaccel input/catlove_720p.webm`
Expected: SW decode → YUV420P→NV12 interleave → correct colors.

- [ ] **Step 5: Test resize/adaptive scale**

Manually resize window. Expected: swapchain recreates, VSR scale adjusts.

- [ ] **Step 6: Test screenshot**

Press S during playback. Expected: `./screenshots/00000_orig.png` and `./screenshots/00000_vsr.png` created.

- [ ] **Step 7: Fix issues and commit**

```bash
git add -A
git commit -m "fix: integration issues from refactor"
```

---

### Task 10: Final cleanup

**Files:**
- `src/client/MainWindow.cpp` — remove stale includes
- `src/core/PlayerCore.cpp` — remove debug prints, tidy

- [ ] **Step 1: Remove unnecessary includes from MainWindow**

MainWindow.cpp should only include:
```cpp
#include "MainWindow.h"
#include "VulkanWidget.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QtGui/qpa/qplatformnativeinterface.h>
#include <cstdio>
#include <png.h>
#include <sys/stat.h>
```

No FFmpeg, no CUDA, no core headers (except api/Player.h via MainWindow.h).

- [ ] **Step 2: Run full build one more time**

Run: `make clean && make -j$(nproc)`
Expected: Clean build, 0 warnings.

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "chore: final cleanup after core/client refactor"
```
