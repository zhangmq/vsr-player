#include "QualityPopup.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QGraphicsOpacityEffect>
#include <cmath>

namespace vsr {

const QStringList QualityPopup::kLabels = {"LOW", "MEDIUM", "HIGH", "ULTRA"};

QualityPopup::QualityPopup(QWidget* parent) : QWidget(parent) {
    setFixedSize(kWidth, kItemHeight * kLabels.size() + 8);
    setMouseTracking(true);
    hide();

    auto* effect = new QGraphicsOpacityEffect(this);
    effect->setOpacity(1.0);
    setGraphicsEffect(effect);
}

void QualityPopup::setCurrentQuality(int quality) {
    currentQuality_ = quality;
    update();
}

void QualityPopup::showAt(const QPoint& anchor) {
    // Position above the anchor point
    int x = anchor.x() - width() / 2;
    int y = anchor.y() - height() - 8;
    if (x < 8) x = 8;
    if (x + width() > parentWidget()->width() - 8)
        x = parentWidget()->width() - width() - 8;
    move(x, y);
    show();
    raise();
}

void QualityPopup::hidePopup() {
    hide();
}

int QualityPopup::itemAtPos(const QPoint& pos) const {
    int idx = (pos.y() - 4) / kItemHeight;
    if (idx >= 0 && idx < kLabels.size()) return idx;
    return -1;
}

void QualityPopup::mouseMoveEvent(QMouseEvent* event) {
    int prev = hoveredIndex_;
    hoveredIndex_ = itemAtPos(event->pos());
    if (prev != hoveredIndex_) update();
}

void QualityPopup::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    int idx = itemAtPos(event->pos());
    if (idx >= 0) {
        emit qualitySelected(idx);
        hidePopup();
    }
}

void QualityPopup::leaveEvent(QEvent*) {
    hoveredIndex_ = -1;
    update();
}

void QualityPopup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Background
    QPainterPath path;
    path.addRoundedRect(rect(), 6, 6);
    painter.fillPath(path, QColor(36, 36, 42, 240));
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.drawPath(path);

    // Items
    QFont font("sans-serif", 12);
    font.setStyleHint(QFont::SansSerif);
    painter.setFont(font);

    for (int i = 0; i < kLabels.size(); i++) {
        QRect r(0, 4 + i * kItemHeight, width(), kItemHeight);
        bool isCurrent = (i == currentQuality_);
        bool isHovered = (i == hoveredIndex_);

        if (isHovered)
            painter.fillRect(r.adjusted(4, 1, -4, -1), QColor(255, 255, 255, 10));

        QColor textColor = isCurrent ? QColor(0xFF, 0xFF, 0xFF)
                           : isHovered ? QColor(0xE0, 0xE0, 0xE0)
                           : QColor(0xA0, 0xA0, 0xA0);
        painter.setPen(textColor);
        painter.drawText(r, Qt::AlignCenter, kLabels[i]);

        // Check mark for current
        if (isCurrent) {
            painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 1.5));
            painter.drawLine(r.right() - 24, r.center().y(),
                             r.right() - 18, r.center().y() + 5);
            painter.drawLine(r.right() - 18, r.center().y() + 5,
                             r.right() - 10, r.center().y() - 5);
        }
    }
}

}  // namespace vsr
