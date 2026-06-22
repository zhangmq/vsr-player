#include "StatusBar.h"

#include <QHBoxLayout>
#include <QLabel>

namespace vsr {

StatusBar::StatusBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    res_label_ = new QLabel("—");
    fps_label_ = new QLabel("—");
    quality_label_ = new QLabel("—");

    layout->addWidget(res_label_);
    layout->addWidget(fps_label_);
    layout->addWidget(quality_label_);
    layout->addStretch();
}

void StatusBar::set_resolution(int in_w, int in_h, int out_w, int out_h) {
    res_label_->setText(QString("%1p → %2p").arg(in_h).arg(out_h));
}

void StatusBar::set_fps(double fps) {
    fps_label_->setText(QString("%1 fps").arg(fps, 0, 'f', 1));
}

void StatusBar::set_quality(const QString& quality) {
    quality_label_->setText(quality);
}

void StatusBar::set_message(const QString& msg) {
    res_label_->setText(msg);
}

}  // namespace vsr
