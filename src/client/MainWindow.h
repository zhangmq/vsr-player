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

/// Qt main window — thin shell over the Player engine.
///
/// MainWindow owns UI controls, the VulkanWidget (Wayland surface),
/// and a Player instance.  All video pipeline logic lives in PlayerCore
/// on its worker thread.  Commands are sent via send_command(); events
/// arrive on the worker thread and are marshaled to the Qt main thread.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Initialize the player engine. Must be called after show()
    /// so the VulkanWidget has a valid wl_surface.
    void init_player(bool use_vsr, Quality quality);

    /// Load a media file (file path or URL).  Sends LOAD_FILE command.
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
