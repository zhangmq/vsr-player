#pragma once

#include <QMainWindow>
#include <QTimer>
#include <memory>
#include <string>

#include "api/Player.h"
#include "IconProvider.h"

class QKeyEvent;

namespace vsr {

class VulkanWidget;
class TopBar;
class CenterPlayButton;
class ControlBar;
class PlaylistPanel;
class QualityPopup;
class PlaylistEngine;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void init_player(bool use_vsr, Quality quality, bool no_hwaccel = false);
    void open_file(const QString& path);
    void open_folder(const QString& path, int depth = 3);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void on_native_window_ready();

private:
    // Player
    void on_player_event(const PlayerEvent& e);
    void request_resize();
    void send_resize();

    // Overlay management
    void showOverlays();
    void hideOverlays();
    void togglePlaylist();
    void toggleQualityPopup();

    // Keyboard shortcuts (called from eventFilter)
    void handleKeyPress(QKeyEvent* event);

    // Vulkan
    VulkanWidget* vulkan_widget_ = nullptr;

    // Overlay components (children of centralWidget)
    TopBar* top_bar_ = nullptr;
    CenterPlayButton* center_play_ = nullptr;
    ControlBar* control_bar_ = nullptr;
    PlaylistPanel* playlist_panel_ = nullptr;
    QualityPopup* quality_popup_ = nullptr;

    // Playlist logic
    PlaylistEngine* playlist_engine_ = nullptr;

    // Auto-hide
    QTimer* hide_timer_ = nullptr;
    bool overlays_visible_ = true;

    // Resize debounce
    QTimer* resize_debounce_ = nullptr;

    // Player engine
    std::unique_ptr<Player> player_;
    bool player_initialized_ = false;

    // Deferred init params
    bool deferred_use_vsr_ = true;
    Quality deferred_quality_ = Quality::HIGH;
    bool deferred_no_hwaccel_ = false;
    QString deferred_file_;
    QString current_file_path_;

    // Screenshot
    int screenshot_counter_ = 0;
    std::string screenshot_dir_ = "./screenshots";
    void save_screenshots(const PlayerEvent& e);
    static void save_png(const std::string& path, const uint8_t* rgb,
                         int w, int h);
};

}  // namespace vsr
