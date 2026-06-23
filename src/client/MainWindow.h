#pragma once

#include <QKeyEvent>
#include <QMainWindow>
#include <QTimer>
#include <memory>
#include <string>

#include "api/Player.h"

class QPushButton;
class QLabel;

namespace vsr {

class VulkanWidget;

/// Qt main window — thin shell over the Player engine.
///
/// MainWindow owns UI controls, the VulkanWidget (Wayland surface),
/// and a Player instance.  All video pipeline logic lives in PlayerCore
/// on its worker thread.  Commands are sent via send_command(); events
/// arrive on the worker thread and are marshaled to the Qt main thread.
///
/// Player initialization is gated on VulkanWidget::nativeWindowReady()
/// — the signal is emitted from showEvent() when the native Wayland
/// surface exists and winId() is valid.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Request player initialization.  If the VulkanWidget's native
    /// window is already ready, init happens immediately; otherwise
    /// it is deferred until nativeWindowReady() fires.
    void init_player(bool use_vsr, Quality quality);

    /// Load a media file.  If the player is not yet initialized,
    /// the path is stored and loaded automatically after init.
    void open_file(const QString& path);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    /// Called when VulkanWidget emits nativeWindowReady().
    void on_native_window_ready();

private:
    void on_player_event(const PlayerEvent& e);
    void request_resize();            // start/restart debounce timer
    void send_resize();               // actual RESIZE command (after debounce)

    // UI
    VulkanWidget* vulkan_widget_ = nullptr;
    QWidget*      overlay_ = nullptr;
    QPushButton*  play_btn_ = nullptr;
    QLabel*       status_label_ = nullptr;

    // Resize debounce (avoids swapchain-rebuild storm during WM animation)
    QTimer* resize_debounce_ = nullptr;

    // Player engine
    std::unique_ptr<Player> player_;
    bool player_initialized_ = false;

    // Deferred init params (set before nativeWindowReady)
    bool   deferred_use_vsr_ = true;
    Quality deferred_quality_ = Quality::HIGH;
    QString deferred_file_;   // file to open after init

    // Screenshot
    int screenshot_counter_ = 0;
    std::string screenshot_dir_ = "./screenshots";
    void save_screenshots(const PlayerEvent& e);
    static void save_png(const std::string& path, const uint8_t* rgb,
                         int w, int h);
};

}  // namespace vsr
