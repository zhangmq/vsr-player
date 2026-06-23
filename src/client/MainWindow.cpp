#include "MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <png.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QtGui/qpa/qplatformnativeinterface.h>

#include "VulkanWidget.h"

namespace vsr {

// ── Constructor / Destructor ──────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("VSR Player");
    setMinimumSize(320, 180);

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
    status_label_->setStyleSheet(
        "color: rgba(255,255,255,0.85); font-size: 13px;");

    bar->addWidget(play_btn_);
    bar->addWidget(status_label_, 1);

    connect(play_btn_, &QPushButton::clicked, this, [this]() {
        if (!player_initialized_) return;
        // Toggle play/pause based on button text
        if (play_btn_->text() == "▶ Play")
            player_->send_command({PlayerCommand::PLAY});
        else
            player_->send_command({PlayerCommand::PAUSE});
    });
}

MainWindow::~MainWindow() {
    if (player_)
        player_->shutdown();
}

// ── Player init ──────────────────────────────────────────────────────

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

void MainWindow::open_file(const QString& path) {
    if (!player_initialized_) return;
    status_label_->setText("Loading...");
    play_btn_->setText("⏸ Pause");
    player_->send_command({PlayerCommand::LOAD_FILE, path.toStdString()});
}

// ── Event handler (Qt main thread) ───────────────────────────────────

void MainWindow::on_player_event(const PlayerEvent& e) {
    switch (e.type) {
    case PlayerEvent::VIDEO_INFO: {
        // Resize window to match video dimensions (clamped to 90% screen)
        if (e.in_width > 0 && e.in_height > 0) {
            auto* screen = QGuiApplication::primaryScreen();
            if (screen) {
                QSize avail = screen->availableGeometry().size();
                int max_w = avail.width() * 9 / 10;
                int max_h = avail.height() * 9 / 10;
                float s = std::min((float)max_w / e.in_width,
                                   (float)max_h / e.in_height);
                resize((int)(e.in_width * s), (int)(e.in_height * s));
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
        const char* mode = e.vsr_active
            ? (e.scale > 1 ? "UPSCALE" : "DENOISE") : "NO-VSR";
        status_label_->setText(
            QString("%1×%2 → %3×%4 x%5 [%6-%7] %8 %9")
                .arg(e.in_width).arg(e.in_height)
                .arg(e.out_width).arg(e.out_height)
                .arg(e.scale).arg(mode).arg(qstr)
                .arg(e.hw_decoding ? "[NVDEC]" : "[SW]")
                .arg(e.has_audio ? "[AUDIO]" : ""));
        break;
    }
    case PlayerEvent::ERROR:
        status_label_->setText(
            QString("Error: %1").arg(QString::fromStdString(e.error_msg)));
        break;
    case PlayerEvent::END_OF_FILE:
        status_label_->setText("End of file");
        break;
    case PlayerEvent::FRAME_CAPTURED:
        save_screenshots(e);
        break;
    default:
        break;
    }
}

// ── Resize ───────────────────────────────────────────────────────────

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
        player_->send_command({PlayerCommand::RESIZE, "", (int64_t)phys_w, (double)phys_h, 0});
}

// ── Key press ────────────────────────────────────────────────────────

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_S && !event->isAutoRepeat()) {
        player_->send_command({PlayerCommand::CAPTURE_FRAME});
        status_label_->setText("Screenshot queued...");
    }
    QMainWindow::keyPressEvent(event);
}

// ── Screenshot ───────────────────────────────────────────────────────

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
    if (!fp) {
        fprintf(stderr, "Screenshot: fopen %s failed\n", path.c_str());
        return;
    }

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

}  // namespace vsr
