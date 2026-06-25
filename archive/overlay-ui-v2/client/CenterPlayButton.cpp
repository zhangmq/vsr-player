#include "CenterPlayButton.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QEnterEvent>

namespace vsr {

CenterPlayButton::CenterPlayButton(QWidget* parent) : QWidget(parent) {
    setFixedSize(kSize, kSize);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    hide();
}

void CenterPlayButton::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    update();
    if (playing)
        hideButton();
    else
        showButton();
}

void CenterPlayButton::showButton() {
    show();
    raise();
}

void CenterPlayButton::hideButton() {
    hide();
}

void CenterPlayButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Circular background
    QPainterPath path;
    path.addEllipse(rect().adjusted(2, 2, -2, -2));
    QColor bg = pressed_ ? QColor(0xFF, 0xFF, 0xFF, 40)
               : hovered_ ? QColor(0xFF, 0xFF, 0xFF, 30)
               : QColor(0, 0, 0, 160);
    painter.fillPath(path, bg);
    painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, 50), 1.5));
    painter.drawPath(path);

    // Icon
    IconName icon = playing_ ? IconName::Pause : IconName::Play;
    QColor iconColor = hovered_ ? QColor(0xFF, 0xFF, 0xFF)
                                : QColor(0xE0, 0xE0, 0xE0);
    QPixmap pm = IconProvider::pixmap(icon, kIconSize, iconColor);
    int x = (width() - kIconSize) / 2;
    int y = (height() - kIconSize) / 2;
    painter.drawPixmap(x, y, pm);
}

void CenterPlayButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed_ = true;
        update();
    }
}

void CenterPlayButton::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && pressed_) {
        pressed_ = false;
        update();
        if (rect().contains(event->pos()))
            emit clicked();
    }
}

void CenterPlayButton::enterEvent(QEnterEvent*) {
    hovered_ = true;
    update();
}

void CenterPlayButton::leaveEvent(QEvent*) {
    hovered_ = false;
    pressed_ = false;
    update();
}

}  // namespace vsr
