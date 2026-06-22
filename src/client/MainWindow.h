#pragma once

#include <QMainWindow>
#include <QTimer>
#include <vector>

#include "api/Player.h"

class QPushButton;
class QLabel;

namespace vsr {

class VulkanWidget;
class Demuxer;
class Decoder;

/// Minimal Qt main window with Vulkan video display.
///
/// For the prototype, the decode loop runs on a QTimer in the main thread.
/// Full architecture will move it to a worker thread via PlayerCore.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void open_file(const QString& path);

private slots:
    void on_timer_tick();

private:
    void setup_ui();

    // UI
    VulkanWidget* vulkan_widget_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QLabel* status_label_ = nullptr;

    // Pipeline (prototype: direct, no PlayerCore threading yet)
    Demuxer* demuxer_ = nullptr;
    Decoder* decoder_ = nullptr;
    void* sws_ctx_ = nullptr;  // SwsContext*

    QTimer* timer_;
    bool playing_ = false;
    bool pipeline_ready_ = false;

    // Frame buffer
    std::vector<uint8_t> rgb_buf_;
    int video_width_ = 0;
    int video_height_ = 0;
    double frame_delay_ms_ = 0.0;
};

}  // namespace vsr
