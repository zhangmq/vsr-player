#include "MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <png.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QtGui/qpa/qplatformnativeinterface.h>

#include "VulkanWidget.h"
#include "TopBar.h"
#include "CenterPlayButton.h"
#include "ControlBar.h"
#include "PlaylistPanel.h"
#include "QualityPopup.h"
#include "PlaylistEngine.h"

namespace vsr {

// ── Constructor / Destructor ──────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("VSR Player");
    setMinimumSize(320, 180);

    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    // ── VulkanWidget — fills entire window ─────────────────────────
    vulkan_widget_ = new VulkanWidget(central);

    // ── Overlay components (all children of central) ───────────────
    top_bar_ = new TopBar(central);
    center_play_ = new CenterPlayButton(central);
    control_bar_ = new ControlBar(central);
    playlist_panel_ = new PlaylistPanel(central);
    quality_popup_ = new QualityPopup(central);

    // ── Playlist engine ────────────────────────────────────────────
    playlist_engine_ = new PlaylistEngine(this);
    playlist_panel_->setEngine(playlist_engine_);

    // ── Auto-hide timer ────────────────────────────────────────────
    hide_timer_ = new QTimer(this);
    hide_timer_->setSingleShot(true);
    hide_timer_->setInterval(3000);
    connect(hide_timer_, &QTimer::timeout, this, &MainWindow::hideOverlays);

    // ── Resize debounce ────────────────────────────────────────────
    resize_debounce_ = new QTimer(this);
    resize_debounce_->setSingleShot(true);
    resize_debounce_->setInterval(200);
    connect(resize_debounce_, &QTimer::timeout, this, &MainWindow::send_resize);

    // ── Signal connections: overlay → MainWindow ───────────────────
    connect(center_play_, &CenterPlayButton::clicked, this, [this]() {
        if (!player_initialized_) return;
        if (center_play_->isPlaying())
            player_->send_command({PlayerCommand::PAUSE});
        else
            player_->send_command({PlayerCommand::PLAY});
    });

    connect(control_bar_, &ControlBar::playPauseClicked, this, [this]() {
        if (!player_initialized_) return;
        player_->send_command({PlayerCommand::PLAY});
        // The STATE_CHANGED event will toggle the actual state
    });

    connect(control_bar_, &ControlBar::stopClicked, this, [this]() {
        if (!player_initialized_) return;
        player_->send_command({PlayerCommand::STOP});
    });

    connect(control_bar_, &ControlBar::previousClicked, this, [this]() {
        QString file = playlist_engine_->previous();
        if (!file.isEmpty()) open_file(file);
    });

    connect(control_bar_, &ControlBar::nextClicked, this, [this]() {
        QString file = playlist_engine_->next();
        if (!file.isEmpty()) open_file(file);
    });

    connect(control_bar_, &ControlBar::seekRequested, this, [this](int64_t pos) {
        if (!player_initialized_) return;
        player_->send_command({PlayerCommand::SEEK, "", pos, 0.0, 0});
    });

    connect(control_bar_, &ControlBar::volumeToggled, this, [this]() {
        if (!player_initialized_) return;
        player_->send_command({PlayerCommand::SET_MUTE});
    });

    connect(control_bar_, &ControlBar::hwToggled, this, [this]() {
        // HW toggle requires decoder rebuild — signal for future impl
        fprintf(stderr, "HW toggle requested (not yet implemented)\n");
    });

    connect(control_bar_, &ControlBar::qualityClicked, this,
            &MainWindow::toggleQualityPopup);

    connect(control_bar_, &ControlBar::playlistToggled, this,
            &MainWindow::togglePlaylist);

    connect(quality_popup_, &QualityPopup::qualitySelected, this,
            [this](int quality) {
                if (!player_initialized_) return;
                static const char* names[] = {"low", "medium", "high", "ultra"};
                player_->send_command({PlayerCommand::SET_QUALITY,
                                       names[quality]});
            });

    connect(playlist_panel_, &PlaylistPanel::fileSelected, this,
            [this](const QString& path, int index) {
                playlist_engine_->setCurrentIndex(index);
                open_file(path);
                playlist_panel_->slideOut();
            });

    connect(playlist_panel_, &PlaylistPanel::closeRequested, this, [this]() {
        playlist_panel_->slideOut();
    });

    // ── Global event filter for auto-hide + keyboard ───────────────
    qApp->installEventFilter(this);

    // Show overlays on startup
    showOverlays();
    center_play_->showButton();
}

MainWindow::~MainWindow() {
    fprintf(stderr, "[~MainWindow] destructor\n");
    qApp->removeEventFilter(this);
    if (player_)
        player_->shutdown();
    fprintf(stderr, "[~MainWindow] done\n");
}

// ── Player init ───────────────────────────────────────────────────────

void MainWindow::init_player(bool use_vsr, Quality quality,
                               bool no_hwaccel) {
    deferred_use_vsr_ = use_vsr;
    deferred_quality_ = quality;
    deferred_no_hwaccel_ = no_hwaccel;

    if (player_initialized_) {
        if (player_)
            player_->send_command({PlayerCommand::SET_QUALITY,
                                   quality == Quality::LOW    ? "low" :
                                   quality == Quality::MEDIUM ? "medium" :
                                   quality == Quality::ULTRA  ? "ultra" : "high"});
        return;
    }

    if (vulkan_widget_->isNativeReady()) {
        on_native_window_ready();
    } else {
        connect(vulkan_widget_, &VulkanWidget::nativeWindowReady,
                this, &MainWindow::on_native_window_ready,
                Qt::SingleShotConnection);
    }
}

void MainWindow::on_native_window_ready() {
    if (player_initialized_) return;

    auto* native = QGuiApplication::platformNativeInterface();
    void* display = native->nativeResourceForIntegration("wl_display");
    void* window  = reinterpret_cast<void*>(vulkan_widget_->winId());

    if (!display || !window) {
        fprintf(stderr, "MainWindow: native window ready but "
                "wl_display=%p wl_surface=%p\n", display, window);
        return;
    }

    player_ = CreatePlayer();
    player_->set_event_callback([this](const PlayerEvent& e) {
        auto copy = std::make_shared<PlayerEvent>(e);
        QMetaObject::invokeMethod(this, [this, copy] {
            on_player_event(*copy);
        }, Qt::QueuedConnection);
    });

    if (!player_->initialize(window, display,
                              deferred_use_vsr_, deferred_quality_,
                              deferred_no_hwaccel_)) {
        fprintf(stderr, "MainWindow: player init failed\n");
        return;
    }

    player_initialized_ = true;
    fprintf(stderr, "MainWindow: player initialized\n");

    if (!deferred_file_.isEmpty()) {
        open_file(deferred_file_);
        deferred_file_.clear();
    }
}

void MainWindow::open_file(const QString& path) {
    current_file_path_ = path;
    if (!player_initialized_) {
        deferred_file_ = path;
        return;
    }
    player_->send_command({PlayerCommand::LOAD_FILE, path.toStdString()});

    // Update playlist index
    int idx = playlist_engine_->indexOf(path);
    if (idx >= 0) {
        playlist_engine_->setCurrentIndex(idx);
        playlist_panel_->setCurrentIndex(idx);
    }
}

void MainWindow::open_folder(const QString& path, int depth) {
    int count = playlist_engine_->scanFolder(path, depth);
    fprintf(stderr, "MainWindow: scanned %s → %d files (depth=%d)\n",
            qPrintable(path), count, depth);

    if (count > 0) {
        // Auto-play the first file
        QString first = playlist_engine_->files().at(0);
        open_file(first);
    }
}

// ── Player events → overlay state ─────────────────────────────────────

void MainWindow::on_player_event(const PlayerEvent& e) {
    switch (e.type) {
    case PlayerEvent::VIDEO_INFO: {
        setWindowTitle(QString("VSR Player — %1 fps").arg(e.fps, 0, 'f', 1));

        // Extract filename from stored path
        QString name = current_file_path_.section('/', -1);
        top_bar_->setFileName(name);

        player_->send_command({PlayerCommand::PLAY});
        break;
    }
    case PlayerEvent::STATE_CHANGED: {
        bool playing = (e.state == PlaybackState::PLAYING);
        center_play_->setPlaying(playing);
        control_bar_->setPlaying(playing);

        if (playing) {
            // If mouse is not over the window, hide overlays
            // (will be handled by auto-hide timer)
        }
        break;
    }
    case PlayerEvent::POSITION_CHANGED: {
        control_bar_->setPosition(e.time_ms);
        control_bar_->setDuration(e.duration_ms);
        break;
    }
    case PlayerEvent::FRAME_INFO: {
        control_bar_->setHwDecoding(e.hw_decoding);
        int q = e.quality == Quality::LOW    ? 0 :
                e.quality == Quality::MEDIUM ? 1 :
                e.quality == Quality::HIGH   ? 2 : 3;
        control_bar_->setQuality(q);
        quality_popup_->setCurrentQuality(q);
        break;
    }
    case PlayerEvent::ERROR:
        top_bar_->setFileName(
            QString("Error: %1").arg(QString::fromStdString(e.error_msg)));
        break;
    case PlayerEvent::END_OF_FILE:
        center_play_->setPlaying(false);
        control_bar_->setPlaying(false);
        // Auto-advance to next file in playlist
        {
            QString next = playlist_engine_->next();
            if (!next.isEmpty()) {
                open_file(next);
            }
        }
        break;
    case PlayerEvent::FRAME_CAPTURED:
        save_screenshots(e);
        break;
    default:
        break;
    }
}

// ── Overlay visibility ────────────────────────────────────────────────

void MainWindow::showOverlays() {
    if (overlays_visible_) {
        hide_timer_->start();
        return;
    }
    overlays_visible_ = true;
    top_bar_->showBar();
    control_bar_->showBar();
    hide_timer_->start();
}

void MainWindow::hideOverlays() {
    if (!overlays_visible_) return;
    if (playlist_panel_->isOpen() || quality_popup_->isVisible())
        return;
    overlays_visible_ = false;
    top_bar_->hideBar();
    control_bar_->hideBar();
}

void MainWindow::togglePlaylist() {
    if (playlist_panel_->isOpen())
        playlist_panel_->slideOut();
    else
        playlist_panel_->slideIn();
}

void MainWindow::toggleQualityPopup() {
    if (quality_popup_->isVisible()) {
        quality_popup_->hidePopup();
    } else {
        // Anchor point: above the quality button in ControlBar
        QPoint anchor(control_bar_->width() - 24, control_bar_->y());
        quality_popup_->showAt(control_bar_->mapToParent(anchor));
    }
}

// ── Global event filter ───────────────────────────────────────────────

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Auto-hide: reset timer on any mouse move or click
    if (event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonPress) {
        showOverlays();
    }

    // Keyboard shortcuts (no focus dependency — works globally in app)
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ke->isAutoRepeat()) {
            handleKeyPress(ke);
            return true;  // consume
        }
    }

    // Click outside QualityPopup to close it
    if (event->type() == QEvent::MouseButtonPress &&
        quality_popup_->isVisible()) {
        auto* me = static_cast<QMouseEvent*>(event);
        QPoint globalPos = me->globalPosition().toPoint();
        QPoint localPos = quality_popup_->mapFromGlobal(globalPos);
        if (!quality_popup_->rect().contains(localPos)) {
            quality_popup_->hidePopup();
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::handleKeyPress(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
        if (player_initialized_)
            player_->send_command({PlayerCommand::PLAY});
        break;
    case Qt::Key_Left:
        if (player_initialized_)
            player_->send_command({PlayerCommand::SEEK, "", -5000, 0.0, 0});
        break;
    case Qt::Key_Right:
        if (player_initialized_)
            player_->send_command({PlayerCommand::SEEK, "", 5000, 0.0, 0});
        break;
    case Qt::Key_P:
        togglePlaylist();
        break;
    case Qt::Key_M:
        if (player_initialized_)
            player_->send_command({PlayerCommand::SET_MUTE});
        break;
    case Qt::Key_S:
        if (player_initialized_ && !event->isAutoRepeat())
            player_->send_command({PlayerCommand::CAPTURE_FRAME});
        break;
    case Qt::Key_Escape:
        if (quality_popup_->isVisible())
            quality_popup_->hidePopup();
        else if (playlist_panel_->isOpen())
            playlist_panel_->slideOut();
        break;
    default:
        break;
    }
}

// ── Resize (debounced) ─────────────────────────────────────────────────

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    const int w = centralWidget()->width();
    const int h = centralWidget()->height();
    vulkan_widget_->setGeometry(0, 0, w, h);

    // Overlay components position themselves in their own resizeEvent
    // CenterPlayButton: center of window
    center_play_->move((w - center_play_->width()) / 2,
                       (h - center_play_->height()) / 2);

    // PlaylistPanel: right side, full height
    playlist_panel_->setGeometry(
        w - playlist_panel_->width(), 0,
        playlist_panel_->width(), h);

    if (player_initialized_)
        request_resize();
}

void MainWindow::request_resize() {
    resize_debounce_->start();
}

void MainWindow::send_resize() {
    qreal dpr = vulkan_widget_->devicePixelRatio();
    int phys_w = (int)(vulkan_widget_->width() * dpr);
    int phys_h = (int)(vulkan_widget_->height() * dpr);
    if (phys_w > 0 && phys_h > 0)
        player_->send_command({PlayerCommand::RESIZE, "", (int64_t)phys_w,
                               (double)phys_h, 0});
}

// ── Graceful shutdown ─────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event) {
    fprintf(stderr, "[closeEvent] player_initialized=%d\n", player_initialized_);
    if (player_initialized_) {
        fprintf(stderr, "[closeEvent] calling player_->shutdown()...\n");
        player_->shutdown();
        fprintf(stderr, "[closeEvent] shutdown returned\n");
    }
    event->accept();
    fprintf(stderr, "[closeEvent] accepted, window will close\n");
}

// ── Screenshot ────────────────────────────────────────────────────────

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
