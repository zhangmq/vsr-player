/// PlayerCore — full playback engine with clean resource lifecycle.
/// Receives commands via variant PlayerCommand, emits events via callback.

#include "PlayerCore.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <cuda.h>

#include "AudioOutput.h"
#include "Decoder.h"
#include "Demuxer.h"
#include "VSRProcessor.h"
#include "utils/CUDAContext.h"
#include "utils/InteropTexture.h"
#include "utils/NV12ToRGB.h"
#include "utils/VulkanRenderer.h"

namespace vsr {

// ── Logging helpers ────────────────────────────────────────────────

static int ctr = 0;
static uint32_t str_hash(const std::string& s) {
    uint32_t h = 5381;
    for (char c : s) h = ((h << 5) + h) + (unsigned char)c;
    return h;
}
static bool log_hash_names() {
    static int v = -1;
    if (v < 0) v = (getenv("VSR_LOG_HASH") != nullptr) ? 1 : 0;
    return v == 1;
}
static std::string log_f(const std::string& path) {
    if (!log_hash_names()) return "'" + path + "'";
    char buf[16]; snprintf(buf, sizeof(buf), "%08x", str_hash(path)); return buf;
}

#define CLOG(fmt, ...) \
    fprintf(stderr, "[core] #%d " fmt "\n", ++ctr, ##__VA_ARGS__)

#define SNAP(label, demux, st, bus, file, sc, qual, pw, ph, fr, mg, pr, ri, fs)  \
    CLOG("snap:%s demux=%p state=%d busy=%d file=%s scale=%d qual=%d "        \
         "phys=%dx%d frame_rdy=%d gate=%d ready=%d in_frame=%d frames_since=%d", \
         label, (void*)(demux), (int)(st), (int)(bus), log_f(file).c_str(),   \
         (int)(sc), (int)(qual), (int)(pw), (int)(ph), (int)(fr),             \
         (int)(mg), (int)(pr), (int)(ri), (int)(fs))

// ── Factory ───────────────────────────────────────────────────────

std::unique_ptr<Player> CreatePlayer() {
    return std::make_unique<PlayerCore>();
}

// ── Lifecycle ─────────────────────────────────────────────────────

PlayerCore::PlayerCore() = default;

PlayerCore::~PlayerCore() {
    running_ = false;
    if (worker_thread_.joinable())
        worker_thread_.join();
    teardown_pipeline();
}

// ── Public API ────────────────────────────────────────────────────

bool PlayerCore::initialize(IVulkanContext* vk,
                             const uint32_t* rgbaFragSpv, size_t rgbaFragSpvLen,
                             const uint32_t* nv12FragSpv, size_t nv12FragSpvLen,
                             const uint32_t* vertSpv, size_t vertSpvLen,
                             int quality, bool no_hwaccel) {
    if (!vk) return false;
    vk_ = vk;
    quality_ = quality;
    no_hwaccel_ = no_hwaccel;

    // Core creates and owns VulkanRenderer. Client only provides
    // IVulkanContext and SPIR-V data through the Player API.
    renderer_ = std::make_unique<VulkanRenderer>();
    renderer_->set_running_flag(&running_);
    if (!renderer_->init_pipelines(*vk_,
            rgbaFragSpv, rgbaFragSpvLen,
            nv12FragSpv, nv12FragSpvLen,
            vertSpv, vertSpvLen)) {
        fprintf(stderr, "PlayerCore: renderer init_pipelines failed\n");
        renderer_.reset();
        return false;
    }
    printf("PlayerCore: initialized (pipelines ready, no textures yet)\n");

    running_ = true;
    worker_thread_ = std::thread(&PlayerCore::run_loop, this);
    return true;
}

void PlayerCore::send_command(PlayerCommand cmd) {
    cmd_queue_.push(std::move(cmd));
}

void PlayerCore::record_frame(void* cb, int w, int h) {
    if (!frame_ready_.load() || !renderer_) return;
    renderer_->record_to_cb(cb, w, h, current_path_);
}

void PlayerCore::set_event_callback(EventCallback cb) {
    event_cb_ = std::move(cb);
}

void PlayerCore::shutdown() {
    // Signal stop BEFORE joining — the worker may be blocked in a
    // deferred reconfig spin-wait that checks running_flag_.
    running_ = false;
    if (worker_thread_.joinable())
        worker_thread_.join();
}

void PlayerCore::emit_event(PlayerEvent e) {
    if (event_cb_)
        event_cb_(e);
}

// ── is_light ───────────────────────────────────────────────────────

bool PlayerCore::is_light(const PlayerCommand& cmd) const {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, CmdPlay>)       return true;
        if constexpr (std::is_same_v<T, CmdPause>)      return true;
        if constexpr (std::is_same_v<T, CmdStop>)       return true;
        if constexpr (std::is_same_v<T, CmdQuit>)       return true;
        if constexpr (std::is_same_v<T, CmdSeek>)       return true;
        if constexpr (std::is_same_v<T, CmdSetVolume>)  return true;
        if constexpr (std::is_same_v<T, CmdSetMute>)    return true;
        if constexpr (std::is_same_v<T, CmdSetHwaccel>) return true;
        if constexpr (std::is_same_v<T, CmdSetSpeed>)   return true;
        if constexpr (std::is_same_v<T, CmdCapture>) return true;
        if constexpr (std::is_same_v<T, CmdSetDenoiseQuality>) return true;
        return false;
    }, cmd);
}

// ── has_pending_work ──────────────────────────────────────────────

bool PlayerCore::has_pending_work() const {
    if (!target_state_.file.empty() && target_state_.file != current_file_) return true;
    if (target_state_.phys_w > 0
        && (target_state_.phys_w != last_phys_w_ || target_state_.phys_h != last_phys_h_))
        return true;
    if (target_state_.scale != user_scale_) return true;
    return false;
}

// ── Main loop ─────────────────────────────────────────────────────

void PlayerCore::run_loop() {
    while (running_) {
        // Drain ALL commands into dispatch
        PlayerCommand cmd;
        while (cmd_queue_.try_pop(cmd)) {
            dispatch(cmd);
        }

        // RESIZE debounce: merge ripe stored dimensions
        auto now = std::chrono::steady_clock::now();
        if (stored_resize_w_ > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_resize_cmd_).count() >= 800) {
            target_state_.merge(CmdResize{stored_resize_w_, stored_resize_h_});
            stored_resize_w_ = stored_resize_h_ = 0;
        }

        // Execute if idle and work pending
        if (!busy_ && has_pending_work()) {
            apply();
        }

        if (state_ == PlaybackState::PLAYING && demuxer_) {
            static int frame_dbg = 0;
            if (frame_dbg++ % 180 == 0)  // ~once per 3s at 60fps
                CLOG("frame: #%d demux=%p state=%d ready=%d",
                     frame_dbg, (void*)demuxer_.get(), (int)state_,
                     frame_ready_.load());
            if (!process_one_frame()) {
                CLOG("frame: EOF state=%d", (int)state_);
                state_ = PlaybackState::STOPPED;
                if (audio_) audio_->stop();
                emit_event({PlayerEvent::END_OF_FILE});
            }
        } else {
            static int idle_ct = 0;
            if (++idle_ct % 600 == 1)  // ~once per 3s
                CLOG("idle: state=%d demux=%p busy=%d target_active=%d",
                     (int)state_, (void*)demuxer_.get(), busy_,
                     has_pending_work());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    fprintf(stderr, "[run_loop] exiting, tearing down pipeline\n");
    teardown_pipeline();
    emit_event({PlayerEvent::STATE_CHANGED, PlaybackState::STOPPED});
    fprintf(stderr, "[run_loop] worker thread done\n");
}

// ── Dispatch ──────────────────────────────────────────────────────

void PlayerCore::dispatch(const PlayerCommand& cmd) {
    if (is_light(cmd)) {
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, CmdPlay>)      cmd_play();
            else if constexpr (std::is_same_v<T, CmdPause>)    cmd_pause();
            else if constexpr (std::is_same_v<T, CmdStop>)     cmd_stop();
            else if constexpr (std::is_same_v<T, CmdQuit>)     cmd_quit();
            else if constexpr (std::is_same_v<T, CmdSeek>)     cmd_seek(arg.position_ms);
            else if constexpr (std::is_same_v<T, CmdSetVolume>) cmd_set_volume(arg.value);
            else if constexpr (std::is_same_v<T, CmdSetMute>)   cmd_set_mute(arg.muted);
            else if constexpr (std::is_same_v<T, CmdSetHwaccel>) cmd_set_hwaccel(arg.enabled);
            else if constexpr (std::is_same_v<T, CmdSetSpeed>)   cmd_set_speed(arg.speed);
            else if constexpr (std::is_same_v<T, CmdCapture>) cmd_capture();
            else if constexpr (std::is_same_v<T, CmdSetDenoiseQuality>) cmd_set_denoise_quality(arg.level);
        }, cmd);
        return;
    }

    // RESIZE: pre-filter debounce — store dimensions, wait 800ms
    if (std::holds_alternative<CmdResize>(cmd)) {
        auto& r = std::get<CmdResize>(cmd);
        stored_resize_w_ = r.phys_w;
        stored_resize_h_ = r.phys_h;
        last_resize_cmd_ = std::chrono::steady_clock::now();
        last_phys_w_ = r.phys_w;
        last_phys_h_ = r.phys_h;
        return;
    }

    // Heavy: merge into target state
    target_state_.merge(cmd);
    // do NOT call apply() here — run_loop handles it after full drain
}

// ── apply ─────────────────────────────────────────────────────────

void PlayerCore::apply() {
    SNAP("apply-enter", demuxer_.get(), state_, busy_, target_state_.file,
         target_state_.scale, (int)quality_, target_state_.phys_w, target_state_.phys_h,
         frame_ready_.load(), renderer_->mutation_gate(), renderer_->is_ready(),
         renderer_->render_in_frame(), renderer_->frames_since_mutation());
    busy_ = true;
    do {
        TargetState snapshot = target_state_;
        CLOG("apply: exec hash=%08x scale=%d phys=%dx%d",
             str_hash(snapshot.file), snapshot.scale,
             snapshot.phys_w, snapshot.phys_h);
        if (!execute(snapshot)) break;

        // Drain commands that arrived during execution.
        PlayerCommand cmd;
        while (cmd_queue_.try_pop(cmd)) {
            dispatch(cmd);
        }

        // Give Qt+GPU time to submit in-flight frames before next round.
        if (has_pending_work()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (has_pending_work());
    SNAP("apply-exit", demuxer_.get(), state_, 0, target_state_.file,
         target_state_.scale, (int)quality_, last_phys_w_, last_phys_h_,
         frame_ready_.load(), renderer_->mutation_gate(), renderer_->is_ready(),
         renderer_->render_in_frame(), renderer_->frames_since_mutation());
    busy_ = false;
}

// ── execute ────────────────────────────────────────────────────────

bool PlayerCore::execute(const TargetState& snapshot) {
    bool file_changed = !snapshot.file.empty() && snapshot.file != current_file_;

    if (file_changed) {
        // ═══ Path A: Full load ═══

        auto new_demuxer = std::make_unique<Demuxer>();
        if (!new_demuxer->open(snapshot.file)) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "Failed to open: " + snapshot.file;
            emit_event(err);
            return false;
        }

        int new_vw = new_demuxer->video_width();
        int new_vh = new_demuxer->video_height();
        if (new_vw <= 0 || new_vh <= 0) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "No video stream in: " + snapshot.file;
            emit_event(err);
            return false;
        }

        auto new_decoder = std::make_unique<Decoder>();
        if (!new_decoder->open(new_demuxer->video_codecpar())) {
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "Decoder open failed: " + snapshot.file;
            emit_event(err);
            return false;
        }
        new_decoder->switch_to_hw(!no_hwaccel_);

        // CUDA context (first load only)
        if (!cuda_ctx_) {
            cuda_ctx_ = std::make_unique<CUDAContext>();
            if (!cuda_ctx_->capture_current()) {
                if (!cuda_ctx_->init(0)) {
                    PlayerEvent err;
                    err.type = PlayerEvent::ERROR;
                    err.error_msg = "CUDA init failed";
                    emit_event(err);
                    return false;
                }
            }
        }
        cuda_ctx_->push();

        if (!cuda_stream_) {
            cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);
        }

        if (!nv12_to_rgb_) {
            nv12_to_rgb_ = std::make_unique<NV12ToRGB>();
            if (!nv12_to_rgb_->compile()) {
                cuda_ctx_->pop();
                PlayerEvent err;
                err.type = PlayerEvent::ERROR;
                err.error_msg = "NV12ToRGB compile failed";
                emit_event(err);
                return false;
            }
        }

        // Sync scale preference from snapshot, then compute effective scale.
        // quality_ persists across file loads (set by light command).
        user_scale_ = snapshot.scale;

        int scale = current_scale_;
        if (user_scale_ == 0) {
            int pw = snapshot.phys_w > 0 ? snapshot.phys_w : last_phys_w_;
            int ph = snapshot.phys_h > 0 ? snapshot.phys_h : last_phys_h_;
            if (pw > 0 && ph > 0)
                scale = compute_adaptive_scale(pw, ph, new_vw, new_vh);
        } else if (user_scale_ == -1) {
            scale = 1;
        } else {
            scale = user_scale_;
        }
        // GPU buffer
        if (rgb_gpu_) {
            cuMemFree((CUdeviceptr)rgb_gpu_);
            rgb_gpu_ = nullptr;
        }
        size_t rgb_bytes = NV12ToRGB::output_size(new_vw, new_vh);
        cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

        // Pre-allocate SW decode staging buffers (lazily freed, reused)
        if (sw_staging_y_) { cuMemFree((CUdeviceptr)sw_staging_y_); sw_staging_y_ = nullptr; }
        if (sw_staging_uv_) { cuMemFree((CUdeviceptr)sw_staging_uv_); sw_staging_uv_ = nullptr; }
        int y_pitch  = ((new_vw + 511) & ~511);
        int uv_pitch = y_pitch;
        cuMemAlloc((CUdeviceptr*)&sw_staging_y_, (size_t)y_pitch * new_vh);
        cuMemAlloc((CUdeviceptr*)&sw_staging_uv_, (size_t)uv_pitch * (new_vh / 2));

        // VSR init
        {
            bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
            if (vsr_ && vsr_active) {
                CLOG("executeA: vsr reset for new file");
                vsr_.reset();
            }
            if (vsr_active && !vsr_) {
                vsr_ = std::make_unique<VSRProcessor>();
                vsr_->set_stream(cuda_stream_);
                int vsr_ow = new_vw * scale;
                int vsr_oh = new_vh * scale;
                CLOG("executeA: vsr init %dx%d->%dx%d scale=%d", new_vw, new_vh, vsr_ow, vsr_oh, scale);
                int vsr_quality = (scale > 1) ? (int)quality_ : denoise_quality_;
                if (!vsr_->init(new_vw, new_vh, vsr_ow, vsr_oh, vsr_quality)) {
                    fprintf(stderr, "PlayerCore: VSR init failed\n");
                    vsr_.reset();
                }
            }
        }

        // Sync CUDA before texture mutation
        CLOG("executeA: cuStreamSync");
        cuStreamSynchronize((CUstream)cuda_stream_);

        // Textures
        CLOG("executeA: reconfigure_all_textures %dx%d scale=%d", new_vw, new_vh, scale);
        renderer_->reconfigure_all_textures(new_vw, new_vh, scale, *vk_);
        CLOG("executeA: textures done");

        // Audio
        auto new_audio = std::make_unique<AudioOutput>();
        if (!new_audio->open(snapshot.file.c_str())) {
            new_audio.reset();
        }

        // Commit
        CLOG("executeA: commit — swap demux/decoder/audio");
        audio_.reset();
        if (new_audio) audio_ = std::move(new_audio);
        audio_started_ = false;
        if (audio_) audio_->set_speed(playback_speed_);
        decoder_.reset();
        decoder_ = std::move(new_decoder);
        demuxer_.reset();
        demuxer_ = std::move(new_demuxer);
        CLOG("executeA: commit done demux=%p", (void*)demuxer_.get());

        current_file_ = snapshot.file;
        video_w_ = new_vw; video_h_ = new_vh;
        video_fps_ = demuxer_->video_fps();
        video_time_base_ = demuxer_->video_time_base();
        duration_ms_ = demuxer_->duration_ms();
        seekable_ = duration_ms_ > 0;
        last_pts_sec_ = -1.0;
        current_pts_sec_ = 0.0;
        pts_fallback_counter_ = 0;
        last_position_emit_ms_ = 0;
        last_render_time_ = std::chrono::steady_clock::now();
        current_scale_ = scale;
        vsr_w_ = new_vw * scale; vsr_h_ = new_vh * scale;

        if (snapshot.phys_w > 0) {
            last_phys_w_ = snapshot.phys_w;
            last_phys_h_ = snapshot.phys_h;
        }
        if (last_phys_w_ > 0) {
            renderer_->resize(last_phys_w_, last_phys_h_);
        }

        cuda_ctx_->pop();

        PlayerEvent info;
        info.type = PlayerEvent::VIDEO_INFO;
        info.in_width = video_w_;
        info.in_height = video_h_;
        info.out_width = vsr_w_;
        info.out_height = vsr_h_;
        info.fps = video_fps_;
        info.scale = current_scale_;
        info.quality = quality_;
        info.duration_ms = duration_ms_;
        info.hw_decoding = decoder_ ? decoder_->is_hardware() : false;
        info.vsr_active = (vsr_ && vsr_->is_ready());
        info.has_audio = (audio_ && audio_->is_active());
        info.seekable = seekable_;
        SNAP("execute-OK", demuxer_.get(), state_, busy_, snapshot.file,
             current_scale_, (int)quality_, last_phys_w_, last_phys_h_,
             frame_ready_.load(), renderer_->mutation_gate(), renderer_->is_ready(),
             renderer_->render_in_frame(), renderer_->frames_since_mutation());
        emit_event(info);
        return true;

    } else {
        // ═══ Path B: Reconfigure only (file unchanged) ═══

        if (!cuda_ctx_ || !renderer_) return false;
        cuda_ctx_->push();

        // Compute effective scale from target user preference.
        user_scale_ = snapshot.scale;

        int scale = current_scale_;
        if (user_scale_ == 0) {
            int pw = snapshot.phys_w > 0 ? snapshot.phys_w : last_phys_w_;
            int ph = snapshot.phys_h > 0 ? snapshot.phys_h : last_phys_h_;
            if (pw > 0 && ph > 0 && video_w_ > 0 && video_h_ > 0)
                scale = compute_adaptive_scale(pw, ph, video_w_, video_h_);
        } else if (user_scale_ == -1) {
            scale = 1;
        } else {
            scale = user_scale_;
        }

        if (scale == current_scale_) {
            if (snapshot.phys_w > 0) {
                last_phys_w_ = snapshot.phys_w;
                last_phys_h_ = snapshot.phys_h;
                renderer_->resize(last_phys_w_, last_phys_h_);
            }
            cuda_ctx_->pop();
            return true;
        }

        vsr_w_ = video_w_ * scale;
        vsr_h_ = video_h_ * scale;
        current_scale_ = scale;

        CLOG("executeB: reconfigure_vsr %dx%d scale=%d", video_w_, video_h_, scale);
        reconfigure_vsr();

        CLOG("executeB: cuStreamSync");
        cuStreamSynchronize((CUstream)cuda_stream_);
        CLOG("executeB: reconfigure_scale %dx%d scale=%d", video_w_, video_h_, scale);
        renderer_->reconfigure_scale(video_w_, video_h_, scale, *vk_);
        CLOG("executeB: scale done");

        if (snapshot.phys_w > 0) {
            last_phys_w_ = snapshot.phys_w;
            last_phys_h_ = snapshot.phys_h;
        }
        renderer_->resize(last_phys_w_, last_phys_h_);

        cuda_ctx_->pop();

        PlayerEvent fi;
        fi.type = PlayerEvent::FRAME_INFO;
        fi.scale = current_scale_;
        fi.quality = quality_;
        emit_event(fi);
        return true;
    }
}

// ── PLAY ──────────────────────────────────────────────────────────

void PlayerCore::cmd_play() {
    CLOG("cmd_play: demux=%p state=%d", (void*)demuxer_.get(), (int)state_);
    if (!demuxer_) { CLOG("cmd_play: SKIP no demuxer"); return; }
    state_ = PlaybackState::PLAYING;
    last_render_time_ = std::chrono::steady_clock::now();
    if (audio_) {
        if (!audio_started_) {
            audio_->start();
            audio_->set_speed(playback_speed_);
            audio_started_ = true;
        } else {
            audio_->resume();
        }
    }
    emit_event({PlayerEvent::STATE_CHANGED, state_});
}

// ── PAUSE ─────────────────────────────────────────────────────────

void PlayerCore::cmd_pause() {
    state_ = PlaybackState::PAUSED;
    if (audio_) audio_->pause();
    emit_event({PlayerEvent::STATE_CHANGED, state_});
}

// ── STOP (playback only — no resource destruction) ────────────────

void PlayerCore::cmd_stop() {
    state_ = PlaybackState::STOPPED;
    if (audio_) {
        audio_->stop();          // joins decode thread, closes PaStream
        audio_started_ = false;  // force full restart on next play
    }
    // Reset all PTS/sync state.
    last_pts_sec_ = -1.0;
    current_pts_sec_ = 0.0;
    pts_fallback_counter_ = 0;
    last_render_time_ = std::chrono::steady_clock::now();
    // Hide stale video frame — render thread will show clear background
    frame_ready_.store(false);
    // Seek to beginning
    cmd_seek(0);
    emit_event({PlayerEvent::STATE_CHANGED, state_});
    // Reset progress bar to 0
    PlayerEvent pe;
    pe.type = PlayerEvent::POSITION_CHANGED;
    pe.time_ms = 0;
    pe.duration_ms = duration_ms_;
    emit_event(pe);
}

// ── SEEK ──────────────────────────────────────────────────────────

void PlayerCore::cmd_seek(int64_t ms) {
    if (!demuxer_ || !seekable_) return;
    // Core-level validation: clamp to valid range
    if (ms < 0) ms = 0;
    if (duration_ms_ > 0 && ms > duration_ms_) ms = duration_ms_;
    CLOG("cmd_seek: %ldms (duration=%ldms)", ms, duration_ms_);

    // Flush decoder BEFORE seeking demuxer — clear old packets first
    if (decoder_) decoder_->flush();

    if (!demuxer_->seek(ms)) {
        CLOG("cmd_seek: demuxer seek failed for %ldms", ms);
        // Continue anyway — seek may still have moved to nearest keyframe
    }

    if (audio_) audio_->seek(ms / 1000.0);

    last_pts_sec_ = ms / 1000.0;
    current_pts_sec_ = ms / 1000.0;
    pts_fallback_counter_ = video_fps_ > 0
        ? (int64_t)(ms / 1000.0 * video_fps_) : 0;
    last_render_time_ = std::chrono::steady_clock::now();
}

// ── SET_QUALITY (VSR only) ────────────────────────────────────────

void PlayerCore::cmd_set_quality(int q) {
    quality_ = q;
    if (vsr_ && vsr_->is_ready())
        vsr_->reconfigure(vsr_w_, vsr_h_, q);
}

// ── SET_DENOISE_QUALITY ─────────────────────────────────────────

void PlayerCore::cmd_set_denoise_quality(int d) {
    denoise_quality_ = d;
    // denoise_quality doesn't affect has_pending_work (not in TargetState),
    // and only takes effect when effective scale is 1.
    int effective = (user_scale_ == 0) ? current_scale_ : (user_scale_ == -1 ? 1 : user_scale_);
    if (effective == 1)
        reconfigure_vsr();
}

// ── SET_VOLUME / SET_MUTE (placeholders) ──────────────────────────

void PlayerCore::cmd_set_volume(double vol) {
    if (vol < 0.0) vol = 0.0;
    if (vol > 1.0) vol = 1.0;
    if (audio_) audio_->set_volume(vol);
}
void PlayerCore::cmd_set_mute(bool) { /* mute now handled by setVolume(0.0) */ }

// ── SET_HWACCEL (toggle NVDEC) ────────────────────────────────────

void PlayerCore::cmd_set_hwaccel(bool enabled) {
    if (no_hwaccel_ == !enabled) return;  // no change
    no_hwaccel_ = !enabled;

    if (!decoder_ || !demuxer_) {
        CLOG("hwaccel: preference changed to %s (applies on next load)",
             enabled ? "HW" : "SW");
        return;
    }

    // Live switch: save position, flush both contexts, seek, switch.
    double saved_pts = current_pts_sec_;
    CLOG("hwaccel: live switch to %s at pts=%.3f", enabled ? "HW" : "SW", saved_pts);

    decoder_->flush();
    decoder_->switch_to_hw(enabled);

    // Seek demuxer to current position so new decoder gets the right stream.
    int64_t ms = (int64_t)(saved_pts * 1000.0);
    demuxer_->seek(ms);
    if (audio_) audio_->seek(saved_pts);

    last_pts_sec_ = saved_pts;
    current_pts_sec_ = saved_pts;
    pts_fallback_counter_ = video_fps_ > 0
        ? (int64_t)(saved_pts * video_fps_) : 0;
    last_render_time_ = std::chrono::steady_clock::now();
}

// ── SET_SPEED ──────────────────────────────────────────────────────

void PlayerCore::cmd_set_speed(double speed) {
    if (speed < 0.1) speed = 0.1;
    if (speed > 4.0) speed = 4.0;
    playback_speed_ = speed;
    if (audio_) audio_->set_speed(speed);
    CLOG("cmd_set_speed: %.2fx", speed);
}

void PlayerCore::cmd_capture() {
    capture_pending_ = true;
    CLOG("cmd_capture: queued");
}

// ── QUIT ──────────────────────────────────────────────────────────

void PlayerCore::cmd_quit() {
    fprintf(stderr, "[run_loop] received QUIT\n");
    running_ = false;
}

// ── Adaptive scale algorithm ──────────────────────────────────────

int PlayerCore::compute_adaptive_scale(int phys_w, int phys_h,
                                       int video_w, int video_h) const {
    bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
    if (!vsr_active || video_w <= 0 || video_h <= 0) return 1;

    // Constrained dimension determines required scale
    // vw/vh >= ww/wh → width-constrained → ceil(ww/vw)
    // vw/vh <  ww/wh → height-constrained → ceil(wh/vh)
    int s;
    if ((double)video_w / video_h >= (double)phys_w / phys_h) {
        s = (phys_w + video_w - 1) / video_w;
    } else {
        s = (phys_h + video_h - 1) / video_h;
    }
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    return s;
}

// ── LOAD_FILE ─────────────────────────────────────────────────────

void PlayerCore::cmd_load_file(const std::string& path) {
    // Phase 1: validate new file can be opened (no mutation yet)
    auto new_demuxer = std::make_unique<Demuxer>();
    if (!new_demuxer->open(path)) {
        PlayerEvent err;
        err.type = PlayerEvent::ERROR;
        err.error_msg = "Failed to open: " + path;
        emit_event(err);
        return;
    }

    int new_vw = new_demuxer->video_width();
    int new_vh = new_demuxer->video_height();
    if (new_vw <= 0 || new_vh <= 0) {
        PlayerEvent err;
        err.type = PlayerEvent::ERROR;
        err.error_msg = "No video stream in: " + path;
        emit_event(err);
        return;
    }

    auto new_decoder = std::make_unique<Decoder>();
    if (!new_decoder->open(new_demuxer->video_codecpar())) {
        PlayerEvent err;
        err.type = PlayerEvent::ERROR;
        err.error_msg = "Decoder open failed: " + path;
        emit_event(err);
        return;
    }
    new_decoder->switch_to_hw(!no_hwaccel_);

    // Phase 2: set up GPU resources
    // CUDA context (first load only)
    if (!cuda_ctx_) {
        cuda_ctx_ = std::make_unique<CUDAContext>();
        if (!cuda_ctx_->capture_current()) {
            if (!cuda_ctx_->init(0)) {
                PlayerEvent err;
                err.type = PlayerEvent::ERROR;
                err.error_msg = "CUDA init failed";
                emit_event(err);
                return;
            }
        }
    }
    cuda_ctx_->push();

    // CUDA stream (first load only)
    if (!cuda_stream_) {
        cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);
    }

    // NV12→RGB kernel (first load only)
    if (!nv12_to_rgb_) {
        nv12_to_rgb_ = std::make_unique<NV12ToRGB>();
        if (!nv12_to_rgb_->compile()) {
            cuda_ctx_->pop();
            PlayerEvent err;
            err.type = PlayerEvent::ERROR;
            err.error_msg = "NV12ToRGB compile failed";
            emit_event(err);
            return;
        }
    }

    // rgb_gpu_ buffer (per-file — destroy old, alloc new)
    if (rgb_gpu_) {
        cuMemFree((CUdeviceptr)rgb_gpu_);
        rgb_gpu_ = nullptr;
    }
    size_t rgb_bytes = NV12ToRGB::output_size(new_vw, new_vh);
    cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

    if (sw_staging_y_) { cuMemFree((CUdeviceptr)sw_staging_y_); sw_staging_y_ = nullptr; }
    if (sw_staging_uv_) { cuMemFree((CUdeviceptr)sw_staging_uv_); sw_staging_uv_ = nullptr; }
    int y_pitch  = ((new_vw + 511) & ~511);
    int uv_pitch = y_pitch;
    cuMemAlloc((CUdeviceptr*)&sw_staging_y_, (size_t)y_pitch * new_vh);
    cuMemAlloc((CUdeviceptr*)&sw_staging_uv_, (size_t)uv_pitch * (new_vh / 2));

    // VSR: init or reconfigure
    int scale = current_scale_;
    if (user_scale_ == 0) {
        // Auto mode: compute adaptive scale from last known window size
        int pw = last_phys_w_ > 0 ? last_phys_w_ : pending_phys_w_;
        int ph = last_phys_h_ > 0 ? last_phys_h_ : pending_phys_h_;
        if (pw > 0 && ph > 0)
            scale = compute_adaptive_scale(pw, ph, new_vw, new_vh);
    } else {
        scale = user_scale_;
    }

    bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
    if (vsr_ && vsr_active) {
        // Input dimensions changed → full re-init
        vsr_.reset();
    }
    if (vsr_active && !vsr_) {
        vsr_ = std::make_unique<VSRProcessor>();
        vsr_->set_stream(cuda_stream_);
        int vsr_ow = new_vw * scale;
        int vsr_oh = new_vh * scale;
        if (!vsr_->init(new_vw, new_vh, vsr_ow, vsr_oh, quality_)) {
            fprintf(stderr, "PlayerCore: VSR init failed\n");
            vsr_.reset();
        }
    }

    // Phase 3: Renderer texture configuration
    // Pipelines are created by the client during init. Core only
    // configures textures here — dimensions depend on the video file.
    renderer_->reconfigure_all_textures(new_vw, new_vh, scale, *vk_);


    // Audio
    auto new_audio = std::make_unique<AudioOutput>();
    if (!new_audio->open(path.c_str())) {
        new_audio.reset();  // audio is optional
    }

    // Phase 4: Commit — destroy old, swap new
    audio_.reset();
    if (new_audio) audio_ = std::move(new_audio);
    audio_started_ = false;
    if (audio_) audio_->set_speed(playback_speed_);
    decoder_.reset();
    decoder_ = std::move(new_decoder);
    demuxer_.reset();
    demuxer_ = std::move(new_demuxer);

    video_w_ = new_vw; video_h_ = new_vh;
    video_fps_ = demuxer_->video_fps();
    video_time_base_ = demuxer_->video_time_base();
    duration_ms_ = demuxer_->duration_ms();
    seekable_ = duration_ms_ > 0;
    last_pts_sec_ = -1.0;
    current_pts_sec_ = 0.0;
    pts_fallback_counter_ = 0;
    last_position_emit_ms_ = 0;
    last_render_time_ = std::chrono::steady_clock::now();
    current_scale_ = scale;
    vsr_w_ = new_vw * scale; vsr_h_ = new_vh * scale;

    // Apply pending resize or last resize
    if (pending_phys_w_ > 0 && pending_phys_h_ > 0) {
        int pw = pending_phys_w_, ph = pending_phys_h_;
        pending_phys_w_ = pending_phys_h_ = 0;
        last_phys_w_ = pw; last_phys_h_ = ph;
    }
    if (last_phys_w_ > 0 && last_phys_h_ > 0) {
        renderer_->resize(last_phys_w_, last_phys_h_);
    }

    cuda_ctx_->pop();

    // Emit video info
    PlayerEvent info;
    info.type = PlayerEvent::VIDEO_INFO;
    info.in_width = video_w_;
    info.in_height = video_h_;
    info.out_width = vsr_w_;
    info.out_height = vsr_h_;
    info.fps = video_fps_;
    info.scale = current_scale_;
    info.quality = quality_;
    info.duration_ms = duration_ms_;
    info.hw_decoding = decoder_ ? decoder_->is_hardware() : false;
    info.vsr_active = (vsr_ && vsr_->is_ready());
    info.has_audio = (audio_ && audio_->is_active());
    info.seekable = seekable_;
    emit_event(info);
}

// ── RESIZE ────────────────────────────────────────────────────────

void PlayerCore::cmd_resize(int phys_w, int phys_h) {
    if (phys_w <= 0 || phys_h <= 0) return;

    last_phys_w_ = phys_w;
    last_phys_h_ = phys_h;

    if (!demuxer_) {
        pending_phys_w_ = phys_w;
        pending_phys_h_ = phys_h;
        return;
    }

    if (!cuda_ctx_) return;
    cuda_ctx_->push();

    if (!renderer_) {
        // First load — renderer not created yet. LOAD_FILE will handle.
        cuda_ctx_->pop();
        return;
    }

    // Locked mode: viewport only
    if (user_scale_ > 0) {
        renderer_->resize(phys_w, phys_h);
        cuda_ctx_->pop();
        return;
    }

    // Auto mode: compute adaptive scale
    int new_scale = compute_adaptive_scale(phys_w, phys_h, video_w_, video_h_);
    if (new_scale != current_scale_) {
        fprintf(stderr, "[core] resize: scale %d→%d (%dx%d)\n",
                current_scale_, new_scale, phys_w, phys_h);
        current_scale_ = new_scale;
        vsr_w_ = video_w_ * new_scale;
        vsr_h_ = video_h_ * new_scale;
        reconfigure_vsr();
        renderer_->reconfigure_scale(video_w_, video_h_, new_scale, *vk_);
    }

    renderer_->resize(phys_w, phys_h);
    cuda_ctx_->pop();
}

// ── SET_SCALE (lock/unlock) ───────────────────────────────────────

void PlayerCore::cmd_set_scale(int s) {
    if (s < -1 || s > 4 || s == 1) return;
    if (s == user_scale_) return;  // no-op
    if (!cuda_ctx_ || !renderer_ || !vsr_) return;

    cuda_ctx_->push();

    user_scale_ = s;
    int new_scale;
    if (s > 0) {
        new_scale = s;  // locked
    } else {
        // Unlock → compute adaptive from last window size
        new_scale = compute_adaptive_scale(last_phys_w_, last_phys_h_,
                                           video_w_, video_h_);
    }

    if (new_scale != current_scale_) {
        fprintf(stderr, "[core] set_scale: %d (user=%d)\n", new_scale, s);
        current_scale_ = new_scale;
        vsr_w_ = video_w_ * new_scale;
        vsr_h_ = video_h_ * new_scale;
        reconfigure_vsr();
        renderer_->reconfigure_scale(video_w_, video_h_, new_scale, *vk_);
    }

    cuda_ctx_->pop();

    PlayerEvent info;
    info.type = PlayerEvent::FRAME_INFO;
    info.scale = current_scale_;
    emit_event(info);
}

// ── VSR reconfigure ───────────────────────────────────────────────

void PlayerCore::reconfigure_vsr() {
    bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
    if (!vsr_active) {
        vsr_.reset();
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
        return;
    }

    int effective = (user_scale_ == 0) ? current_scale_ : (user_scale_ == -1 ? 1 : user_scale_);
    if (effective > 1) {
        // Upscale mode
        vsr_w_ = video_w_ * effective;
        vsr_h_ = video_h_ * effective;
        if (vsr_ && vsr_->is_ready()) {
            vsr_->reconfigure(vsr_w_, vsr_h_, quality_);
        } else {
            vsr_ = std::make_unique<VSRProcessor>();
            vsr_->set_stream(cuda_stream_);
            if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, quality_)) {
                fprintf(stderr, "PlayerCore: VSR init (upscale) failed\n");
                vsr_.reset();
            }
        }
    } else if (effective == 1 && denoise_quality_ != -1) {
        // Denoise mode
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
        if (vsr_ && vsr_->is_ready()) {
            vsr_->reconfigure(vsr_w_, vsr_h_, denoise_quality_);
        } else {
            vsr_ = std::make_unique<VSRProcessor>();
            vsr_->set_stream(cuda_stream_);
            if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, denoise_quality_)) {
                fprintf(stderr, "PlayerCore: VSR init (denoise) failed\n");
                vsr_.reset();
            }
        }
    } else {
        // VSR off — effective scale=1 and denoise off
        vsr_.reset();
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
    }
}

// ── Tear down file-specific resources ─────────────────────────────

void PlayerCore::teardown_file_resources() {
    if (audio_) {
        audio_->stop();
        audio_.reset();
    }
    decoder_.reset();
    demuxer_.reset();
    if (rgb_gpu_) {
        cuda_ctx_->push();
        cuMemFree((CUdeviceptr)rgb_gpu_);
        rgb_gpu_ = nullptr;
        cuda_ctx_->pop();
    }
    if (sw_staging_y_) {
        cuda_ctx_->push();
        cuMemFree((CUdeviceptr)sw_staging_y_);
        sw_staging_y_ = nullptr;
        cuda_ctx_->pop();
    }
    if (sw_staging_uv_) {
        cuda_ctx_->push();
        cuMemFree((CUdeviceptr)sw_staging_uv_);
        sw_staging_uv_ = nullptr;
        cuda_ctx_->pop();
    }
    current_file_.clear();
    video_w_ = video_h_ = 0;
    video_fps_ = 0.0;
    video_time_base_ = 0.0;
    last_pts_sec_ = -1.0;
    current_pts_sec_ = 0.0;
    pts_fallback_counter_ = 0;
    audio_started_ = false;
}

// ── Full teardown (QUIT only) ─────────────────────────────────────

void PlayerCore::teardown_pipeline() {
    if (cuda_ctx_)
        cuda_ctx_->push();
    // Destroy Vulkan resources before releasing CUDA context
    if (renderer_) {
        renderer_->release(*vk_);
        renderer_.reset();
    }


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
    if (sw_staging_y_) { cuMemFree((CUdeviceptr)sw_staging_y_); sw_staging_y_ = nullptr; }
    if (sw_staging_uv_) { cuMemFree((CUdeviceptr)sw_staging_uv_); sw_staging_uv_ = nullptr; }
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

    current_file_.clear();
    video_w_ = video_h_ = 0;
    video_fps_ = 0.0;
    video_time_base_ = 0.0;
    last_pts_sec_ = -1.0;
    current_pts_sec_ = 0.0;
    pts_fallback_counter_ = 0;
    audio_started_ = false;
    current_scale_ = 1;
    user_scale_ = 0;
}

// ── Frame processing ──────────────────────────────────────────────

bool PlayerCore::process_one_frame() {
    if (!cuda_ctx_ || !demuxer_ || !decoder_) return false;
    cuda_ctx_->push();

    // 1. Read packet
    AVPacket* pkt = demuxer_->read_packet();
    if (!pkt) {
        cuda_ctx_->pop();
        return false;  // EOF
    }

    // Audio packets: skip (AudioOutput handles audio in its own thread)
    if (pkt->stream_index == demuxer_->audio_stream_index() && audio_) {
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

    decoded_frames_++;

    // 3. Format check
    bool is_hw = (hw_frame->format == AV_PIX_FMT_CUDA);
    if (!is_hw && hw_frame->format != AV_PIX_FMT_NV12 &&
        hw_frame->format != AV_PIX_FMT_YUV420P) {
        decoder_->release_frame(hw_frame);
        cuda_ctx_->pop();
        return true;
    }

    // 4. YUV420P → NV12 interleave (SW decode with planar output)
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

    // 5. A/V sync check BEFORE expensive GPU work (VSR + D2D).
    // Uses real decoder PTS (mpv/VLC model), with speed scaling and
    // fallback to frame counter when PTS is invalid (e.g. NVDEC frames).
    static int64_t pts_dbg = 0;
    double frame_pts_sec;
    if (hw_frame->pts != AV_NOPTS_VALUE) {
        frame_pts_sec = (double)hw_frame->pts * demuxer_->video_time_base();
        if (++pts_dbg % 180 == 0)
            CLOG("pts-OK #%ld frame_pts=%.3f", pts_dbg, frame_pts_sec);
    } else {
        frame_pts_sec = (double)pts_fallback_counter_ / video_fps_;
        pts_fallback_counter_++;
        if (pts_fallback_counter_ % 180 == 0)
            CLOG("pts-FALLBACK #%ld est=%.3f", pts_fallback_counter_, frame_pts_sec);
    }
    current_pts_sec_ = frame_pts_sec;

    if (audio_ && audio_->is_active()) {
        // Clock returns content-time (not wall-clock). At 0.5x speed,
        // the clock advances at 0.5 content-seconds per real-second.
        // Video PTS is also content-time, so both timelines stay aligned
        // at all speeds — no clock_bias_ needed.
        double clock = audio_->clock_sec();
        double delay = frame_pts_sec - clock;

        if (delay < -0.050) {
            // Video behind audio > 50ms → drop frame (skip VSR+D2D)
            dropped_frames_++;
            decoder_->release_frame(hw_frame);
            cuda_ctx_->pop();
            return true;
        }
        if (delay > 0.002) {
            // Video ahead of audio → sleep before GPU work.
            // At speed<1.0, the real-time needed to close the gap is
            // larger — cap accommodates 50ms real sleep regardless.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(
                    std::min(delay * 1000.0 / playback_speed_, 50.0))));
        }
    } else {
        // No audio: pace by PTS delta between consecutive frames
        if (last_pts_sec_ >= 0.0) {
            double pts_diff = (frame_pts_sec - last_pts_sec_) / playback_speed_;
            auto target = last_render_time_
                + std::chrono::duration<double>(pts_diff);
            auto now = std::chrono::steady_clock::now();
            if (now < target)
                std::this_thread::sleep_for(target - now);
        }
        last_pts_sec_ = frame_pts_sec;
        last_render_time_ = std::chrono::steady_clock::now();
    }

    // 6. NV12→RGB GPU kernel
    {
        uint8_t* gpu_y  = y_plane;
        uint8_t* gpu_uv = uv_plane;
        if (!is_hw) {
            size_t y_sz  = (size_t)y_pitch * video_h_;
            size_t uv_sz = (size_t)uv_pitch * (video_h_ / 2);
            cuMemcpyHtoDAsync((CUdeviceptr)sw_staging_y_, y_plane, y_sz, (CUstream)cuda_stream_);
            cuMemcpyHtoDAsync((CUdeviceptr)sw_staging_uv_, uv_plane, uv_sz, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
            gpu_y  = (uint8_t*)sw_staging_y_;
            gpu_uv = (uint8_t*)sw_staging_uv_;
        }

        nv12_to_rgb_->convert(gpu_y, y_pitch, gpu_uv, uv_pitch,
                               video_w_, video_h_,
                               rgb_gpu_, cuda_stream_);
    }

    // 7. VSR processing
    void* vsr_out_ptr = nullptr;
    int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
    if (vsr_ && vsr_->is_ready()) {
        vsr_->process(rgb_gpu_, &vsr_out_ptr, &vsr_out_w, &vsr_out_h,
                      &vsr_out_pitch);
    }

    // 8. D2D copy → InteropTexture
    if (renderer_ && renderer_->is_ready()) {
        if (vsr_out_ptr) {
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
            auto& yTex  = renderer_->yInterop();
            auto& uvTex = renderer_->uvInterop();

            CUDA_MEMCPY2D copyY = {};
            copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyY.dstDevice     = yTex.cudaPtr();
            copyY.dstPitch      = yTex.cudaPitch();
            copyY.srcPitch      = (size_t)y_pitch;
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
            copyUV.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copyUV.dstDevice     = uvTex.cudaPtr();
            copyUV.dstPitch      = uvTex.cudaPitch();
            copyUV.srcPitch      = (size_t)uv_pitch;
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

        current_path_ = vsr_out_ptr ? Path::VSR : Path::NOVSR;
        frame_ready_.store(true);
    }

    // 9. Screenshot capture
    if (capture_pending_) {
        capture_pending_ = false;

        int npixels = video_w_ * video_h_;
        size_t plane_bytes = (size_t)npixels * sizeof(float);

        // Original frame: rgb_gpu_ (float32 CHW) → uint8 RGB
        {
            std::vector<float> rgb_cpu(npixels * 3);
            cuMemcpyDtoH(rgb_cpu.data(), (CUdeviceptr)rgb_gpu_, plane_bytes * 3);

            capture_orig_buf_.resize((size_t)npixels * 3);
            float* rp = rgb_cpu.data();
            float* gp = rgb_cpu.data() + npixels;
            float* bp = rgb_cpu.data() + npixels * 2;
            for (int i = 0; i < npixels; i++) {
                capture_orig_buf_[i * 3 + 0] =
                    (uint8_t)std::clamp((int)(rp[i] * 255.0f), 0, 255);
                capture_orig_buf_[i * 3 + 1] =
                    (uint8_t)std::clamp((int)(gp[i] * 255.0f), 0, 255);
                capture_orig_buf_[i * 3 + 2] =
                    (uint8_t)std::clamp((int)(bp[i] * 255.0f), 0, 255);
            }
        }

        // Upscaled frame: VSR output (RGBA) → uint8 RGB
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
                uint8_t* dst = capture_vsr_buf_.data() +
                               (size_t)row * vsr_out_w * 3;
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

    // 9. Audio-path PTS tracking (video-only path tracks in step 5)
    if (audio_ && audio_->is_active()) {
        last_pts_sec_ = frame_pts_sec;
        last_render_time_ = std::chrono::steady_clock::now();
    }

    // 10. Periodic position update (real PTS, not frame_count_/fps estimate)
    int64_t pos_ms = (int64_t)(frame_pts_sec * 1000.0);
    int64_t diff_ms = pos_ms > last_position_emit_ms_
                      ? pos_ms - last_position_emit_ms_
                      : last_position_emit_ms_ - pos_ms;
    if (diff_ms >= 200) {
        last_position_emit_ms_ = pos_ms;
        PlayerEvent pe;
        pe.type = PlayerEvent::POSITION_CHANGED;
        pe.time_ms = pos_ms;
        pe.duration_ms = duration_ms_;
        emit_event(pe);
    }

    // 11. Frame info event (throttled to every 30 frames ~0.5s at 60fps)
    frame_count_++;
    if (frame_count_ % 30 == 0) {
        PlayerEvent fi;
        fi.type = PlayerEvent::FRAME_INFO;
        fi.in_width = video_w_;
        fi.in_height = video_h_;
        fi.out_width = vsr_w_;
        fi.out_height = vsr_h_;
        fi.fps = video_fps_;
        fi.scale = current_scale_;
        fi.quality = quality_;
        fi.denoise = denoise_quality_;
        fi.hw_decoding = decoder_->is_hardware();
        fi.vsr_active = (vsr_ && vsr_->is_ready());
        fi.has_audio = (audio_ && audio_->is_active());
        fi.speed = playback_speed_;
        fi.phys_w = last_phys_w_;
        fi.phys_h = last_phys_h_;
        fi.decoded_frames = decoded_frames_;
        fi.dropped_frames = dropped_frames_;

        // Render FPS: frames_since_last / elapsed
        auto now = std::chrono::steady_clock::now();
        if (info_start_frame_ > 0) {
            double elapsed = std::chrono::duration<double>(
                now - last_info_time_).count();
            fi.render_fps = elapsed > 0 ? (frame_count_ - info_start_frame_) / elapsed : 0;
        }
        last_info_time_ = now;
        info_start_frame_ = frame_count_;

        fi.frame_idx = frame_count_;

        // GPU name (cached)
        if (gpu_name_.empty()) {
            char gname[128];
            if (cuDeviceGetName(gname, sizeof(gname), 0) == CUDA_SUCCESS)
                gpu_name_ = gname;
        }
        fi.gpu_name = gpu_name_;

        // VRAM usage
        {
            size_t free_bytes = 0, total_bytes = 0;
            if (cuMemGetInfo(&free_bytes, &total_bytes) == CUDA_SUCCESS) {
                fi.vram_used_mb = (int)((total_bytes - free_bytes) / (1024 * 1024));
                fi.vram_total_mb = (int)(total_bytes / (1024 * 1024));
            }
        }

        // Audio info
        if (audio_ && audio_->is_active()) {
            fi.audio_sr = audio_->sample_rate();
            fi.audio_ch = audio_->channels();
        }

        // Codec & pixel format
        {
            int codec_id = decoder_->active_codec_id();
            if (codec_id) {
                const char* cn = avcodec_get_name((AVCodecID)codec_id);
                if (cn) fi.codec_name = cn;
            }
            const char* pfn = decoder_->pix_fmt_name();
            if (pfn) fi.pix_fmt_name = pfn;
        }

        emit_event(fi);
    }

    decoder_->release_frame(hw_frame);
    cuda_ctx_->pop();
    return true;
}

}  // namespace vsr
