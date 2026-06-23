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

// Generated SPIR-V shaders (from Makefile: glslc + xxd)
#include "video_vert_spv.h"
#include "video_frag_spv.h"
#include "nv12_frag_spv.h"

#include "AudioOutput.h"
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

void PlayerCore::shutdown() {
    if (!running_) return;
    // Signal the worker to leave render_frame() as soon as possible.
    // This avoids the main thread timing out while the worker is blocked
    // in Vulkan presentation calls (vkAcquireNextImageKHR / vkQueuePresentKHR).
    shutting_down_ = true;
    send_command({PlayerCommand::QUIT});

    // Wait for worker to finish, with timeout to prevent hang on exit
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(3);
    while (!thread_done_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (thread_done_.load()) {
        if (worker_thread_.joinable())
            worker_thread_.join();
    } else {
        fprintf(stderr, "PlayerCore: worker thread did not exit "
                "within 3s — detaching\n");
        worker_thread_.detach();
    }
    running_ = false;
}

void PlayerCore::emit_event(PlayerEvent e) {
    if (event_cb_)
        event_cb_(e);
}

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

    thread_done_ = true;
    emit_event({PlayerEvent::STATE_CHANGED, PlaybackState::STOPPED});
}

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
        break;  // unimplemented commands are no-ops for now
    }
    return true;
}

// ── LOAD_FILE ────────────────────────────────────────────────────────

void PlayerCore::cmd_load_file(const std::string& path) {
    teardown_pipeline();

    if (!build_pipeline(path)) {
        PlayerEvent err;
        err.type = PlayerEvent::ERROR;
        err.error_msg = "Failed to open file: " + path;
        emit_event(err);
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
    info.has_audio = (audio_ && audio_->is_active());
    info.seekable = seekable_;
    emit_event(info);

    // Apply pending resize if any (client may have resized before load finished)
    if (pending_phys_w_ > 0 && pending_phys_h_ > 0) {
        int pw = pending_phys_w_, ph = pending_phys_h_;
        pending_phys_w_ = pending_phys_h_ = 0;
        cmd_resize(pw, ph);
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

    // ── NV12→RGB kernel ──
    nv12_to_rgb_ = std::make_unique<NV12ToRGB>();
    if (!nv12_to_rgb_->compile()) {
        fprintf(stderr, "PlayerCore: NV12ToRGB compile failed\n");
        cuda_ctx_->pop();
        return false;
    }

    // ── GPU output buffer (float32 CHW RGB) ──
    size_t rgb_bytes = NV12ToRGB::output_size(video_w_, video_h_);
    cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

    // ── Audio (file-based: AudioOutput opens its own FFmpeg instance) ──
    {
        int audio_idx = demuxer_->audio_stream_index();
        if (audio_idx >= 0) {
            audio_ = std::make_unique<AudioOutput>();
            if (!audio_->open(path.c_str())) {
                fprintf(stderr, "PlayerCore: audio open failed\n");
                audio_.reset();
            }
        }
    }

    // ── VSR (initial scale 1x; will be updated by RESIZE after pipelines init) ──
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

void PlayerCore::teardown_pipeline() {
    if (cuda_ctx_)
        cuda_ctx_->push();

    // Release Vulkan resources first (InteropTextures need device alive)
    if (renderer_) {
        renderer_->release();
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
    audio_started_ = false;
}

// ── Simple command handlers ──────────────────────────────────────────

void PlayerCore::cmd_play() {
    if (!demuxer_) return;
    state_ = PlaybackState::PLAYING;
    if (audio_) {
        if (!audio_started_) {
            audio_->start();
            audio_started_ = true;
        } else {
            audio_->resume();
        }
    }
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
    if (decoder_) decoder_->flush();
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
    // Placeholder — AudioOutput volume not yet implemented
    (void)vol;
}

void PlayerCore::cmd_set_mute(bool mute) {
    // Placeholder — AudioOutput mute not yet implemented
    (void)mute;
}

// ── RESIZE ───────────────────────────────────────────────────────────

void PlayerCore::cmd_resize(int phys_w, int phys_h) {
    if (phys_w <= 0 || phys_h <= 0) return;

    // Defer if pipeline not ready yet
    if (!demuxer_) {
        pending_phys_w_ = phys_w;
        pending_phys_h_ = phys_h;
        return;
    }

    if (!cuda_ctx_) return;
    cuda_ctx_->push();

    // ── Init VulkanRenderer on first resize ──
    if (!renderer_) {
        renderer_ = std::make_unique<VulkanRenderer>();
        if (!renderer_->init(native_window_, native_display_)) {
            fprintf(stderr, "PlayerCore: VulkanRenderer init failed\n");
            cuda_ctx_->pop();
            return;
        }
    }

    // ── Adaptive scale ──
    int new_scale = 1;
    if (use_vsr_ && video_w_ > 0 && video_h_ > 0) {
        int sw = (phys_w + video_w_ - 1) / video_w_;
        int sh = (phys_h + video_h_ - 1) / video_h_;
        new_scale = std::clamp(std::min(sw, sh), 1, 4);
    }
    bool scale_changed = (new_scale != current_scale_);

    // Store desired size. Actual swapchain resize is handled by
    // VulkanRenderer::render_frame() via VK_ERROR_OUT_OF_DATE_KHR
    // recovery — that path properly coordinates with the compositor.
    renderer_->resize(phys_w, phys_h);

    // First time: create swapchain + pipelines
    if (!renderer_->is_ready() || !renderer_->swapchainWidth()) {
        current_scale_ = new_scale;
        vsr_w_ = video_w_ * current_scale_;
        vsr_h_ = video_h_ * current_scale_;
        reconfigure_vsr();
        renderer_->set_shader_data(
            reinterpret_cast<const uint32_t*>(video_frag_spv),
            video_frag_spv_len,
            reinterpret_cast<const uint32_t*>(nv12_frag_spv),
            nv12_frag_spv_len,
            reinterpret_cast<const uint32_t*>(video_vert_spv),
            video_vert_spv_len);
        renderer_->init_pipelines_with_saved_spv(
            video_w_, video_h_, current_scale_, phys_w, phys_h);
    } else if (scale_changed) {
        // Scale changed — recreate InteropTextures (swapchain untouched)
        current_scale_ = new_scale;
        vsr_w_ = video_w_ * current_scale_;
        vsr_h_ = video_h_ * current_scale_;
        reconfigure_vsr();
        renderer_->init_pipelines_with_saved_spv(
            video_w_, video_h_, current_scale_, phys_w, phys_h);
    }

    cuda_ctx_->pop();
}

// ── VSR reconfigure ──────────────────────────────────────────────────

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

// ── Frame processing ─────────────────────────────────────────────────

bool PlayerCore::process_one_frame() {
    if (!cuda_ctx_ || !demuxer_ || !decoder_) return false;
    cuda_ctx_->push();

    // 1. Read packet
    AVPacket* pkt = demuxer_->read_packet();
    if (!pkt) {
        cuda_ctx_->pop();
        return false;  // EOF
    }

    // Audio packets: decode inline and feed to audio sink
    if (pkt->stream_index == demuxer_->audio_stream_index() && audio_) {
        // Skip for now — AudioOutput handles audio in its own thread
        // with its own FFmpeg instance. PCM path will come later.
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

        static bool yuv420p_logged = false;
        if (!yuv420p_logged) {
            yuv420p_logged = true;
            fprintf(stderr, "PlayerCore: YUV420P→NV12 interleave "
                    "(%dx%d, u_pitch=%d v_pitch=%d nv12_pitch=%d)\n",
                    uv_w, uv_h, u_pitch, v_pitch, nv12_uv_pitch);
        }
    }

    // 5. NV12→RGB GPU kernel
    {
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

    // 6. VSR processing
    void* vsr_out_ptr = nullptr;
    int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
    if (vsr_ && vsr_->is_ready()) {
        vsr_->process(rgb_gpu_, &vsr_out_ptr, &vsr_out_w, &vsr_out_h,
                      &vsr_out_pitch);
    }

    // 7. D2D copy → InteropTexture + Vulkan render.
    // If shutting down, skip all rendering — the worker should exit its
    // main loop as quickly as possible to process QUIT and tear down.
    if (renderer_ && renderer_->is_ready() && !shutting_down_) {
        if (vsr_out_ptr) {
            // ── VSR path: RGBA → rgbaInterop ──
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
            // ── NO-VSR path: NV12 → yInterop + uvInterop ──
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

        renderer_->render_frame(vsr_out_ptr ? Path::VSR : Path::NOVSR);
    }

    // 8. Screenshot capture
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

    // 9. A/V sync (audio master clock)
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

}  // namespace vsr
