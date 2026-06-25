#include "IconButton.h"

#include <QPainter>
#include <QEnterEvent>
#include <QMouseEvent>

namespace vsr {

IconButton::IconButton(IconName icon, int iconSize, QWidget* parent)
    : QWidget(parent), icon_(icon), iconSize_(iconSize) {
    int pad = 6;
    setFixedSize(iconSize + pad * 2, iconSize + pad * 2);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void IconButton::setIcon(IconName icon) {
    icon_ = icon;
    update();
}

void IconButton::setIconSize(int size) {
    iconSize_ = size;
    int pad = 6;
    setFixedSize(size + pad * 2, size + pad * 2);
    update();
}

void IconButton::setColor(const QColor& normal, const QColor& hover) {
    normalColor_ = normal;
    hoverColor_ = hover;
    update();
}

void IconButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor c = (hovered_ || pressed_) ? hoverColor_ : normalColor_;
    if (pressed_) c = c.darker(120);

    QPixmap pm = IconProvider::pixmap(icon_, iconSize_, c);
    int x = (width() - iconSize_) / 2;
    int y = (height() - iconSize_) / 2;
    painter.drawPixmap(x, y, pm);
}

void IconButton::enterEvent(QEnterEvent*) {
    hovered_ = true;
    update();
}

void IconButton::leaveEvent(QEvent*) {
    hovered_ = false;
    pressed_ = false;
    update();
}

void IconButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed_ = true;
        update();
    }
}

void IconButton::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && pressed_) {
        pressed_ = false;
        update();
        if (rect().contains(event->pos()))
            emit clicked();
    }
}

}  // namespace vsr
