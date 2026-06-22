#pragma once

#include <QMainWindow>

class QListView;
class QStringListModel;
class QPushButton;
class QSlider;
class QLabel;

namespace vsr {

class VulkanWidget;
class PlayerProxy;

/// Main application window — Qt client for the VSR player.
///
/// Layout:
///   ┌──────────────────────────────────┐
///   │  PlaylistPanel  │  VulkanWidget  │
///   │                 │  (video area)  │
///   ├─────────────────┴───────────────┤
///   │  ControlBar (play/pause/seek)   │
///   ├─────────────────────────────────┤
///   │  StatusBar (fps, res, quality)  │
///   └─────────────────────────────────┘
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Open a video file or playlist.
    void open_file(const QString& path);

private slots:
    void on_play_pause();
    void on_stop();
    void on_seek(int64_t ms);
    void on_quality_changed(int index);
    void on_playlist_item_activated(const QModelIndex& index);

private:
    void setup_ui();
    void setup_connections();
    void connect_player_events();

    // UI components
    QListView* playlist_view_ = nullptr;
    QStringListModel* playlist_model_ = nullptr;
    VulkanWidget* vulkan_widget_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QLabel* status_label_ = nullptr;

    // Player proxy (bridge to libvsrplayer)
    PlayerProxy* player_proxy_ = nullptr;

    bool playing_ = false;
};

}  // namespace vsr
