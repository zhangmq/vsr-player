#include "MainWindow.h"

#include <chrono>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "Decoder.h"
#include "Demuxer.h"
#include "VulkanWidget.h"

namespace vsr {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("VSR Player");
    resize(1280, 720);
    setup_ui();
}

MainWindow::~MainWindow() {
    if (decoder_) decoder_->close();
    delete decoder_;
    delete demuxer_;
    if (sws_ctx_) sws_freeContext((SwsContext*)sws_ctx_);
}

void MainWindow::setup_ui() {
    // No layout — we manage positions manually for overlay effect.
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    // Let children fill by resizing in resizeEvent.

    // Video display — fills entire window
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

    // Timer for frame pacing
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::on_timer_tick);

    // Play button — load and start
    connect(play_btn_, &QPushButton::clicked, this, [this]() {
        if (!pipeline_ready_) return;
        playing_ = !playing_;
        play_btn_->setText(playing_ ? "⏸ Pause" : "▶ Play");
        if (playing_) timer_->start(1);
        else timer_->stop();
    });
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Video fills entire window
    const int w = centralWidget()->width();
    const int h = centralWidget()->height();
    vulkan_widget_->setGeometry(0, 0, w, h);
    // Overlay bar at the bottom, with margins
    const int margin = 12;
    overlay_->setGeometry(margin, h - overlay_->height() - margin,
                          w - margin * 2, overlay_->height());
}

void MainWindow::open_file(const QString& path) {
    status_label_->setText("Opening...");

    // Create pipeline
    demuxer_ = new Demuxer();
    if (!demuxer_->open(path.toStdString())) {
        status_label_->setText("Failed to open file");
        return;
    }

    decoder_ = new Decoder();
    if (!decoder_->open(demuxer_->video_codecpar())) {
        status_label_->setText("Failed to open decoder");
        return;
    }

    video_width_ = demuxer_->video_width();
    video_height_ = demuxer_->video_height();
    frame_delay_ms_ = 1000.0 / demuxer_->video_fps();

    // NV12→RGB24 converter
    sws_ctx_ = sws_getContext(video_width_, video_height_, AV_PIX_FMT_NV12,
                              video_width_, video_height_, AV_PIX_FMT_RGB24,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);

    // RGB buffer
    rgb_buf_.resize(video_width_ * video_height_ * 3);

    pipeline_ready_ = true;
    status_label_->setText(QString("%1x%2 @ %3fps %4")
        .arg(video_width_).arg(video_height_)
        .arg(demuxer_->video_fps(), 0, 'f', 1)
        .arg(decoder_->is_hardware() ? "[NVDEC]" : "[SW]"));

    // Auto-start playback
    playing_ = true;
    play_btn_->setText("⏸ Pause");
    timer_->start(1);
}

void MainWindow::on_timer_tick() {
    if (!playing_ || !pipeline_ready_) return;

    // Read next packet
    AVPacket* pkt = demuxer_->read_packet();
    if (!pkt) {
        // EOF
        playing_ = false;
        timer_->stop();
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

    // GPU→CPU transfer
    AVFrame* sw_frame = nullptr;
    if (hw_frame->format == AV_PIX_FMT_CUDA) {
        sw_frame = av_frame_alloc();
        av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    } else {
        sw_frame = av_frame_clone(hw_frame);
    }

    // NV12→RGB24
    uint8_t* dst[1] = { rgb_buf_.data() };
    int dst_stride[1] = { video_width_ * 3 };
    sws_scale((SwsContext*)sws_ctx_, sw_frame->data, sw_frame->linesize,
              0, video_height_, dst, dst_stride);

    // Display via Vulkan
    vulkan_widget_->present_frame(rgb_buf_.data(), video_width_, video_height_);

    av_frame_free(&sw_frame);
    decoder_->release_frame(hw_frame);

    // Frame pacing
    static auto last_frame = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    last_frame = now;
}

}  // namespace vsr
