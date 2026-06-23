#include "MainWindow.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <cuda.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include "AudioOutput.h"
#include "Decoder.h"
#include "Demuxer.h"
#include "utils/CUDAContext.h"
#include "utils/NV12ToRGB.h"
#include "VSRProcessor.h"
#include "VulkanWidget.h"

namespace vsr {

// ── Constructor / Destructor ──────────────────────────────────────────

MainWindow::MainWindow(bool use_vsr, Quality quality, QWidget* parent) : QMainWindow(parent) {
    use_vsr_ = use_vsr;
    quality_ = quality;
    setWindowTitle(use_vsr_ ? "VSR Player" : "VSR Player (no VSR)");
    resize(1280, 720);
    setup_ui();
}

MainWindow::~MainWindow() {
    // Release Vulkan renderer (including InteropTextures) before CUDA context
    if (vulkan_widget_) vulkan_widget_->releaseRenderer();

    if (audio_) audio_->stop();
    if (decoder_) decoder_->close();
    if (rgb_gpu_) cuMemFree((CUdeviceptr)rgb_gpu_);
    if (cuda_stream_) cuStreamDestroy((CUstream)cuda_stream_);
    delete nv12_to_rgb_;
    delete vsr_;
    delete audio_;
    delete decoder_;
    delete demuxer_;
    delete cuda_ctx_;
}

// ── UI ────────────────────────────────────────────────────────────────

void MainWindow::setup_ui() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    vulkan_widget_ = new VulkanWidget(central);

    // Overlay control bar — semi-transparent, bottom-aligned
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

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::on_timer_tick);

    connect(play_btn_, &QPushButton::clicked, this, [this]() {
        if (!pipeline_ready_) return;
        playing_ = !playing_;
        play_btn_->setText(playing_ ? "⏸ Pause" : "▶ Play");
        if (playing_) {
            timer_->start(1);
            if (audio_) audio_->resume();
        } else {
            timer_->stop();
            if (audio_) audio_->pause();
        }
    });
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    const int w = centralWidget()->width();
    const int h = centralWidget()->height();
    vulkan_widget_->setGeometry(0, 0, w, h);
    const int margin = 12;
    overlay_->setGeometry(margin, h - overlay_->height() - margin,
                          w - margin * 2, overlay_->height());

    // Adaptive scale on resize
    if (pipeline_ready_) {
        fprintf(stderr, "VSR: resizeEvent — widget=%dx%d video=%dx%d\n",
                vulkan_widget_->width(), vulkan_widget_->height(),
                video_width_, video_height_);
        update_scale();
    }
}

// ── Adaptive scale ────────────────────────────────────────────────────

int MainWindow::adaptive_scale(int in_w, int in_h, int win_w, int win_h) const {
    if (in_w <= 0 || in_h <= 0) return 1;
    int sw = (win_w + in_w - 1) / in_w;
    int sh = (win_h + in_h - 1) / in_h;
    return std::clamp(std::min(sw, sh), 1, 4);
}

void MainWindow::update_scale() {
    // No-op when VSR is disabled
    if (!use_vsr_) return;

    int ww = vulkan_widget_->width();
    int wh = vulkan_widget_->height();
    qreal dpr = vulkan_widget_->devicePixelRatio();
    int phys_w = (int)(ww * dpr);
    int phys_h = (int)(wh * dpr);
    fprintf(stderr, "VSR: update_scale — widget=%dx%d dpr=%.1f phys=%dx%d video=%dx%d current=%d\n",
            ww, wh, dpr, phys_w, phys_h, video_width_, video_height_, current_scale_);
    if (phys_w <= 0 || phys_h <= 0 || video_width_ <= 0 || video_height_ <= 0) return;

    int new_scale = adaptive_scale(video_width_, video_height_, phys_w, phys_h);
    fprintf(stderr, "VSR: new_scale=%d current=%d (sw=%d sh=%d)\n",
            new_scale, current_scale_,
            (phys_w + video_width_ - 1) / video_width_,
            (phys_h + video_height_ - 1) / video_height_);
    if (new_scale == current_scale_) return;

    apply_scale(new_scale);
}

void MainWindow::apply_scale(int scale) {
    fprintf(stderr, "VSR: apply_scale(%d) — current=%d\n", scale, current_scale_);
    if (scale == current_scale_) return;
    current_scale_ = scale;
    pending_scale_ = 0;

    int out_w = video_width_ * scale;
    int out_h = video_height_ * scale;

    fprintf(stderr, "VSR: applying scale x%d (%dx%d → %dx%d)\n",
            scale, video_width_, video_height_, out_w, out_h);

    // ── Reconfigure VSR (creates new NvVFX instance per Python reference) ──
    // Python vsr_pipeline.py: "A fresh VideoSuperRes is created because
    // calling load() after run() on the same instance produces an NvVFX
    // invalid-parameter error (code -7)."
    if (vsr_) {
        vsr_->reconfigure(out_w, out_h, quality_);
    }

    vsr_w_ = out_w;
    vsr_h_ = out_h;

    // Reinitialize Vulkan pipelines with new VSR output dimensions
    // (CUDA context is current — pushed from open_file() and stays valid)
    if (vulkan_widget_) {
        vulkan_widget_->init_pipelines(video_width_, video_height_, current_scale_);
    }

    // Update status bar
    {
        const char* qstr = quality_ == Quality::LOW ? "LOW" :
                           quality_ == Quality::MEDIUM ? "MEDIUM" :
                           quality_ == Quality::HIGH ? "HIGH" : "ULTRA";
        bool vsr_active = (vsr_ && vsr_->is_ready());
        const char* mode = vsr_active ? (current_scale_ > 1 ? "UPSCALE" : "DENOISE") : "NO-VSR";
        if (vsr_active) {
            status_label_->setText(QString("%1×%2 → %3×%4 x%5 [%6-%7] %8 %9")
                .arg(video_width_).arg(video_height_)
                .arg(vsr_w_).arg(vsr_h_)
                .arg(current_scale_)
                .arg(mode).arg(qstr)
                .arg(decoder_->is_hardware() ? "[NVDEC]" : "[SW]")
                .arg(audio_ && audio_->is_active() ? "[AUDIO]" : ""));
        } else {
            status_label_->setText(QString("%1×%2 [%3] %4 %5")
                .arg(video_width_).arg(video_height_)
                .arg(mode)
                .arg(decoder_->is_hardware() ? "[NVDEC]" : "[SW]")
                .arg(audio_ && audio_->is_active() ? "[AUDIO]" : ""));
        }
    }
}

// ── Open file ─────────────────────────────────────────────────────────

void MainWindow::open_file(const QString& path) {
    status_label_->setText("Opening...");

    // Clean up previous pipeline
    if (audio_) { audio_->stop(); delete audio_; audio_ = nullptr; }
    if (vsr_)   { delete vsr_;   vsr_   = nullptr; }
    if (nv12_to_rgb_) { delete nv12_to_rgb_; nv12_to_rgb_ = nullptr; }
    if (decoder_) { decoder_->close(); delete decoder_; decoder_ = nullptr; }
    if (demuxer_) { delete demuxer_; demuxer_ = nullptr; }
    if (cuda_ctx_) { delete cuda_ctx_; cuda_ctx_ = nullptr; }
    if (rgb_gpu_)  { cuMemFree((CUdeviceptr)rgb_gpu_);  rgb_gpu_  = nullptr; }
    if (cuda_stream_) { cuStreamDestroy((CUstream)cuda_stream_); cuda_stream_ = nullptr; }
    pipelines_initialized_ = false;

    // ── Demuxer ──
    demuxer_ = new Demuxer();
    if (!demuxer_->open(path.toStdString())) {
        status_label_->setText("Failed to open file");
        return;
    }

    // ── Decoder (NVDEC hwaccel or software) ──
    decoder_ = new Decoder();
    if (!decoder_->open(demuxer_->video_codecpar(), no_hwaccel_)) {
        status_label_->setText("Failed to open decoder");
        return;
    }

    video_width_ = demuxer_->video_width();
    video_height_ = demuxer_->video_height();
    frame_delay_ms_ = 1000.0 / demuxer_->video_fps();

    // ── CUDA context (capture from FFmpeg/Decoder's context) ──
    cuda_ctx_ = new CUDAContext();
    if (!cuda_ctx_->capture_current()) {
        // Try creating a fresh one
        if (!cuda_ctx_->init(0)) {
            status_label_->setText("CUDA init failed");
            return;
        }
    }

    // ── CUDA stream for async operations ──
    cuStreamCreate((CUstream*)&cuda_stream_, CU_STREAM_NON_BLOCKING);

    // ── NV12→RGB GPU converter ──
    nv12_to_rgb_ = new NV12ToRGB();
    if (!nv12_to_rgb_->compile()) {
        status_label_->setText("NV12→RGB kernel compile failed");
        return;
    }

    // ── Allocate GPU buffers ──
    size_t rgb_bytes = NV12ToRGB::output_size(video_width_, video_height_);
    cuMemAlloc((CUdeviceptr*)&rgb_gpu_, rgb_bytes);

    // Initial VSR scale — use physical pixels (DPR-aware).
    // Only meaningful when VSR is enabled; with --no-vsr scale is always 1.
    if (use_vsr_) {
        int ww = (int)(vulkan_widget_->width() * vulkan_widget_->devicePixelRatio());
        int wh = (int)(vulkan_widget_->height() * vulkan_widget_->devicePixelRatio());
        fprintf(stderr, "VSR: initial scale — in=%dx%d phys=%dx%d\n",
                video_width_, video_height_, ww, wh);
        current_scale_ = adaptive_scale(video_width_, video_height_, ww, wh);
    } else {
        current_scale_ = 1;
    }
    vsr_w_ = video_width_ * current_scale_;
    vsr_h_ = video_height_ * current_scale_;

    // ── VSR processor (skip with --no-vsr) ──
    if (use_vsr_) {
        vsr_ = new VSRProcessor();
        vsr_->set_stream(cuda_stream_);  // share stream with NV12ToRGB kernel
        if (!vsr_->init(video_width_, video_height_, vsr_w_, vsr_h_, quality_)) {
            fprintf(stderr, "VSR: init failed — displaying native resolution\n");
            delete vsr_;
            vsr_ = nullptr;
            vsr_w_ = video_width_;
            vsr_h_ = video_height_;
        }
    } else {
        fprintf(stderr, "VSR: [NO-VSR] %dx%d → %dx%d (interop NV12 display)\n",
                video_width_, video_height_, video_width_, video_height_);
        vsr_w_ = video_width_;
        vsr_h_ = video_height_;
    }

    // ── Audio ──
    audio_ = new AudioOutput();
    if (audio_->open(path.toStdString().c_str())) {
        audio_->start();
    } else {
        // No audio track — that's fine, video-only playback
        delete audio_;
        audio_ = nullptr;
    }

    // ── Status ──
    pipeline_ready_ = true;
    {
        const char* qstr = quality_ == Quality::LOW ? "LOW" :
                           quality_ == Quality::MEDIUM ? "MEDIUM" :
                           quality_ == Quality::HIGH ? "HIGH" : "ULTRA";
        bool vsr_active = (vsr_ && vsr_->is_ready());
        const char* mode = vsr_active ? (current_scale_ > 1 ? "UPSCALE" : "DENOISE") : "NO-VSR";
        if (vsr_active) {
            status_label_->setText(QString("%1×%2 → %3×%4 x%5 [%6-%7] %8 %9")
                .arg(video_width_).arg(video_height_)
                .arg(vsr_w_).arg(vsr_h_)
                .arg(current_scale_)
                .arg(mode).arg(qstr)
                .arg(decoder_->is_hardware() ? "[NVDEC]" : "[SW]")
                .arg(audio_ && audio_->is_active() ? "[AUDIO]" : ""));
        } else {
            status_label_->setText(QString("%1×%2 [%3] %4 %5")
                .arg(video_width_).arg(video_height_)
                .arg(mode)
                .arg(decoder_->is_hardware() ? "[NVDEC]" : "[SW]")
                .arg(audio_ && audio_->is_active() ? "[AUDIO]" : ""));
        }
    }

    // Auto-start playback
    playing_ = true;
    play_btn_->setText("⏸ Pause");
    timer_->start(1);

    // Defer a scale re-check: Qt layout may not have finalized widget
    // geometry yet when open_file() runs (called synchronously after show()).
    // resizeEvent will fire when the window first appears, correcting the
    // scale if needed.
    QTimer::singleShot(50, this, [this]() {
        if (pipeline_ready_) update_scale();
    });
}

// ── Timer tick — main decode/VSR/render loop ──────────────────────────

void MainWindow::on_timer_tick() {
    if (!playing_ || !pipeline_ready_) return;

    // Read next packet
    AVPacket* pkt = demuxer_->read_packet();
    if (!pkt) {
        // EOF
        playing_ = false;
        timer_->stop();
        if (audio_) audio_->stop();
        status_label_->setText("End of file");
        return;
    }

    if (pkt->stream_index != demuxer_->video_stream_index()) {
        av_packet_free(&pkt);
        return;
    }

    // Decode
    decoder_->send_packet(pkt->data, pkt->size, pkt->pts);
    av_packet_free(&pkt);

    AVFrame* hw_frame = decoder_->receive_frame();
    if (!hw_frame) return;

    // HW decode: NV12 CUDA pointers. SW decode: NV12 CPU pointers.
    bool is_hw = (hw_frame->format == AV_PIX_FMT_CUDA);
    if (!is_hw && hw_frame->format != AV_PIX_FMT_NV12 &&
        hw_frame->format != AV_PIX_FMT_YUV420P) {
        // Unsupported pixel format (shouldn't happen)
        decoder_->release_frame(hw_frame);
        return;
    }

    cuda_ctx_->push();

    // Init Vulkan pipelines on first frame (now we know video dimensions)
    if (!pipelines_initialized_) {
        fprintf(stderr, "MainWindow: initializing Vulkan pipelines "
                "(%dx%d, scale=%d)\n",
                video_width_, video_height_, current_scale_);
        vulkan_widget_->init_pipelines(video_width_, video_height_,
                                        current_scale_);
        pipelines_initialized_ = true;
    }

    // NV12 planes from decoder (HW: GPU pointers, SW: CPU pointers).
    // YUV420P (planar, e.g. libdav1d SW output) must be converted to NV12
    // (semi-planar interleaved UV) before the H2D/D2D paths below.
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
        int uv_w = video_width_ / 2;
        int uv_h = video_height_ / 2;
        // New UV row stride = W bytes (W/2 pairs × 2 bytes), no padding
        int nv12_uv_pitch = video_width_;
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
            fprintf(stderr, "MainWindow: YUV420P→NV12 interleave "
                    "(%dx%d, u_pitch=%d v_pitch=%d nv12_pitch=%d)\n",
                    uv_w, uv_h, u_pitch, v_pitch, nv12_uv_pitch);
        }
    }

    if (vsr_) {
        // ── VSR path: NV12 → float32 RGB → VSR → RGBA InteropTexture ──
        // For SW decode: H2D copy NV12 planes to temp GPU buffers first.
        CUdeviceptr tmp_y = 0, tmp_uv = 0;
        if (!is_hw) {
            size_t y_sz  = (size_t)y_pitch * video_height_;
            size_t uv_sz = (size_t)uv_pitch * (video_height_ / 2);
            cuMemAlloc(&tmp_y, y_sz);
            cuMemAlloc(&tmp_uv, uv_sz);
            cuMemcpyHtoDAsync(tmp_y,  y_plane,  y_sz,  (CUstream)cuda_stream_);
            cuMemcpyHtoDAsync(tmp_uv, uv_plane, uv_sz, (CUstream)cuda_stream_);
            cuStreamSynchronize((CUstream)cuda_stream_);
        }

        uint8_t* gpu_y  = is_hw ? y_plane  : (uint8_t*)tmp_y;
        uint8_t* gpu_uv = is_hw ? uv_plane : (uint8_t*)tmp_uv;

        nv12_to_rgb_->convert(gpu_y, y_pitch, gpu_uv, uv_pitch,
                               video_width_, video_height_,
                               rgb_gpu_, cuda_stream_);

        if (!is_hw) { cuMemFree(tmp_y); cuMemFree(tmp_uv); }

        void* vsr_out_ptr = nullptr;
        int vsr_out_w = 0, vsr_out_h = 0, vsr_out_pitch = 0;
        bool vsr_ok = vsr_->process(rgb_gpu_, &vsr_out_ptr, &vsr_out_w, &vsr_out_h,
                                     &vsr_out_pitch);

        if (vsr_ok && vsr_out_ptr) {
            size_t row_bytes = (size_t)vsr_out_w * 4;
            auto& rgbaTex = vulkan_widget_->rgbaInterop();

            // D2D copy VSR output → InteropTexture (GPU→GPU)
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
        }
    } else {
        // ── NO-VSR path: NV12 planes → InteropTextures (D2D or H2D) ──
        auto& yTex  = vulkan_widget_->yInterop();
        auto& uvTex = vulkan_widget_->uvInterop();

        CUDA_MEMCPY2D copyY = {};
        copyY.srcPitch      = (size_t)y_pitch;
        copyY.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copyY.dstDevice     = yTex.cudaPtr();
        copyY.dstPitch      = yTex.cudaPitch();
        copyY.WidthInBytes  = (size_t)video_width_;
        copyY.Height        = (size_t)video_height_;
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
        copyUV.WidthInBytes  = (size_t)video_width_;
        copyUV.Height        = (size_t)(video_height_ / 2);
        if (is_hw) {
            copyUV.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copyUV.srcDevice     = (CUdeviceptr)uv_plane;
        } else {
            copyUV.srcMemoryType = CU_MEMORYTYPE_HOST;
            copyUV.srcHost       = uv_plane;
        }
        cuMemcpy2DAsync(&copyUV, (CUstream)cuda_stream_);
        cuStreamSynchronize((CUstream)cuda_stream_);

        vsr_w_ = video_width_;
        vsr_h_ = video_height_;
    }

    // Render via Vulkan (CUDA context still current for potential
    // pipeline recreation on resize)
    vulkan_widget_->render_frame(vsr_ ? Path::VSR : Path::NOVSR);

    cuda_ctx_->pop();
    decoder_->release_frame(hw_frame);

    // A/V sync — audio master clock (mpv/VLC model)
    if (audio_ && audio_->is_active()) {
        double clock = audio_->clock_sec();
        // Estimate PTS from frame count
        static int frame_count = 0;
        frame_count++;
        double pts_sec = frame_count / demuxer_->video_fps();
        double delay = pts_sec - clock;
        if (delay > 0.002) {
            // Video ahead — sleep (capped at 50ms to keep UI responsive)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(std::min(delay * 1000.0, 50.0))));
        }
    }
}

}  // namespace vsr
