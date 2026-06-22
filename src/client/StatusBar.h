#pragma once

#include <QWidget>

class QLabel;

namespace vsr {

/// Status bar showing current playback info: resolution, quality, fps.
class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);

    void set_resolution(int in_w, int in_h, int out_w, int out_h);
    void set_fps(double fps);
    void set_quality(const QString& quality);
    void set_message(const QString& msg);

private:
    QLabel* res_label_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QLabel* quality_label_ = nullptr;
};

}  // namespace vsr
