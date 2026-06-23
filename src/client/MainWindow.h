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
class CUDAContext;
class NV12ToRGB;
class VSRProcessor;
class AudioOutput;

/// Qt main window with Vulkan video, VSR processing, audio playback,
/// and overlay controls.
///
/// Prototype pipeline: decode on QTimer in main thread, GPU NV12→RGB,
/// NvVFX VSR, PortAudio audio.  Full architecture will move to PlayerCore.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool use_vsr = true, Quality quality = Quality::HIGH,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    void open_file(const QString& path);
    void set_no_hwaccel(bool v) { no_hwaccel_ = v; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void on_timer_tick();

private:
    void setup_ui();
    void update_scale();
    void apply_scale(int scale);
    int adaptive_scale(int in_w, int in_h, int win_w, int win_h) const;

    // Video display (fills entire window)
    VulkanWidget* vulkan_widget_ = nullptr;

    // Overlay controls (floating on top of video)
    QWidget* overlay_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QLabel* status_label_ = nullptr;

    // Pipeline (prototype: direct, no PlayerCore threading yet)
    Demuxer* demuxer_ = nullptr;
    Decoder* decoder_ = nullptr;

    // GPU pipeline
    CUDAContext* cuda_ctx_ = nullptr;
    NV12ToRGB* nv12_to_rgb_ = nullptr;
    VSRProcessor* vsr_ = nullptr;
    AudioOutput* audio_ = nullptr;

    QTimer* timer_;
    QTimer* resize_timer_ = nullptr;  // debounce resize → VSR reconfigure
    int pending_scale_ = 0;           // scale to apply after debounce (0 = none)
    bool playing_ = false;
    bool pipeline_ready_ = false;
    bool use_vsr_ = true;
    bool no_hwaccel_ = false;

    // Video info
    int video_width_ = 0;
    int video_height_ = 0;
    double frame_delay_ms_ = 0.0;

    // GPU buffers (device memory)
    float* rgb_gpu_ = nullptr;    // NV12ToRGB output: (3,H,W) float32
    void* cuda_stream_ = nullptr;


    int vsr_w_ = 0, vsr_h_ = 0;   // current VSR output dimensions

    // Adaptive scale
    int current_scale_ = 1;
    Quality quality_ = Quality::HIGH;
    bool pipelines_initialized_ = false;
};

}  // namespace vsr
