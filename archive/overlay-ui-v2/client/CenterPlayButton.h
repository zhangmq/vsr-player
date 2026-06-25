#pragma once

#include <QWidget>

#include "IconProvider.h"

namespace vsr {

class CenterPlayButton : public QWidget {
    Q_OBJECT

public:
    explicit CenterPlayButton(QWidget* parent = nullptr);

    void setPlaying(bool playing);
    bool isPlaying() const { return playing_; }

    void showButton();
    void hideButton();

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool playing_ = false;
    bool hovered_ = false;
    bool pressed_ = false;

    static constexpr int kSize = 64;
    static constexpr int kIconSize = 32;
};

}  // namespace vsr
