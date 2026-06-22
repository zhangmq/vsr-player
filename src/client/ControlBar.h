#pragma once

#include <cstdint>

#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;

namespace vsr {

/// Bottom control bar: play/pause, stop, seek slider, volume.
class ControlBar : public QWidget {
    Q_OBJECT

public:
    explicit ControlBar(QWidget* parent = nullptr);

    void set_duration(int64_t ms);
    void set_position(int64_t ms);
    void set_playing(bool playing);
    void set_volume(double vol);

signals:
    void play_pause_clicked();
    void stop_clicked();
    void seek_requested(int64_t ms);
    void volume_changed(double vol);

private:
    QPushButton* play_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QLabel* time_label_ = nullptr;
    int64_t duration_ms_ = 0;
};

}  // namespace vsr
