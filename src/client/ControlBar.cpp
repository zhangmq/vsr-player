#include "ControlBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

namespace vsr {

ControlBar::ControlBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);

    play_btn_ = new QPushButton("▶");
    stop_btn_ = new QPushButton("■");
    seek_slider_ = new QSlider(Qt::Horizontal);
    volume_slider_ = new QSlider(Qt::Horizontal);
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(100);
    time_label_ = new QLabel("00:00 / 00:00");

    layout->addWidget(play_btn_);
    layout->addWidget(stop_btn_);
    layout->addWidget(seek_slider_, 1);
    layout->addWidget(time_label_);
    layout->addWidget(new QLabel("Vol"));
    layout->addWidget(volume_slider_);

    connect(play_btn_, &QPushButton::clicked, this, &ControlBar::play_pause_clicked);
    connect(stop_btn_, &QPushButton::clicked, this, &ControlBar::stop_clicked);
}

void ControlBar::set_duration(int64_t ms) { duration_ms_ = ms; }
void ControlBar::set_position(int64_t) {}
void ControlBar::set_playing(bool playing) {
    play_btn_->setText(playing ? "⏸" : "▶");
}
void ControlBar::set_volume(double) {}

}  // namespace vsr
