#include "PlaylistPanel.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QTimer>

namespace vsr {

PlaylistPanel::PlaylistPanel(QWidget* parent) : QWidget(parent) {
    setFixedWidth(kWidth);
    setMouseTracking(true);
    hide();

    slideAnim_ = new QPropertyAnimation(this, "pos", this);
    slideAnim_->setDuration(250);
    slideAnim_->setEasingCurve(QEasingCurve::OutCubic);
}

void PlaylistPanel::setEngine(PlaylistEngine* engine) {
    engine_ = engine;
    update();
}

void PlaylistPanel::setCurrentIndex(int index) {
    currentIndex_ = index;
    // Auto-scroll to keep current item visible
    int visible = (height() - kHeaderHeight) / kItemHeight;
    int firstVisible = scrollOffset_;
    if (index < firstVisible)
        scrollOffset_ = index;
    else if (index >= firstVisible + visible)
        scrollOffset_ = index - visible + 1;
    update();
}

// ── Slide animation ────────────────────────────────────────────────────

void PlaylistPanel::slideIn() {
    open_ = true;
    if (parentWidget()) {
        int x = parentWidget()->width() - kWidth;
        setGeometry(x, 0, kWidth, parentWidget()->height());
    }
    show();
    raise();

    QPoint from = pos();
    from.setX(parentWidget() ? parentWidget()->width() : from.x() + kWidth);

    slideAnim_->stop();
    slideAnim_->setStartValue(from);
    slideAnim_->setEndValue(pos());
    slideAnim_->start();
}

void PlaylistPanel::slideOut() {
    open_ = false;
    QPoint to = pos();
    to.setX(parentWidget() ? parentWidget()->width() : to.x() + kWidth);

    slideAnim_->stop();
    slideAnim_->setStartValue(pos());
    slideAnim_->setEndValue(to);
    slideAnim_->start();

    QTimer::singleShot(260, this, [this]() {
        if (!open_) hide();
    });
}

// ── Layout ─────────────────────────────────────────────────────────────

QRect PlaylistPanel::itemRect(int index) const {
    int row = index - scrollOffset_;
    int y = kHeaderHeight + row * kItemHeight;
    return QRect(0, y, width(), kItemHeight);
}

int PlaylistPanel::itemAtPos(const QPoint& pos) const {
    if (pos.y() < kHeaderHeight) return -1;
    int row = (pos.y() - kHeaderHeight) / kItemHeight;
    int idx = row + scrollOffset_;
    if (!engine_ || idx < 0 || idx >= engine_->files().size()) return -1;
    return idx;
}

// ── Mouse ──────────────────────────────────────────────────────────────

void PlaylistPanel::mouseMoveEvent(QMouseEvent* event) {
    int prev = hoveredIndex_;
    hoveredIndex_ = itemAtPos(event->pos());
    if (prev != hoveredIndex_) update();
}

void PlaylistPanel::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;

    // Close button area (top-right)
    QRect closeBtn(width() - kCloseBtnSize - 12, 10, kCloseBtnSize, kCloseBtnSize);
    if (closeBtn.contains(event->pos())) {
        emit closeRequested();
        return;
    }

    int idx = itemAtPos(event->pos());
    if (idx >= 0) {
        emit fileSelected(engine_->files().at(idx), idx);
    }
}

void PlaylistPanel::wheelEvent(QWheelEvent* event) {
    if (!engine_) return;
    int maxScroll = qMax(0, engine_->files().size() - (height() - kHeaderHeight) / kItemHeight);
    int delta = event->angleDelta().y() / 120;
    scrollOffset_ = std::clamp(scrollOffset_ - delta, 0, maxScroll);
    update();
}

// ── Paint ──────────────────────────────────────────────────────────────

void PlaylistPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Panel background
    painter.fillRect(rect(), QColor(24, 24, 28, 240));

    // Header
    painter.setPen(QColor(0xE0, 0xE0, 0xE0));
    QFont headerFont("sans-serif", 14);
    headerFont.setStyleHint(QFont::SansSerif);
    headerFont.setBold(true);
    painter.setFont(headerFont);

    QString headerText = engine_ ? QString("Playlist (%1)").arg(engine_->files().size())
                                 : "Playlist";
    painter.drawText(QRect(16, 0, width() - 48, kHeaderHeight),
                     Qt::AlignVCenter | Qt::AlignLeft, headerText);

    // Close button (X)
    QRect closeBtn(width() - kCloseBtnSize - 12, (kHeaderHeight - kCloseBtnSize) / 2,
                   kCloseBtnSize, kCloseBtnSize);
    painter.setPen(QPen(QColor(0xA0, 0xA0, 0xA0), 1.5));
    painter.drawLine(closeBtn.topLeft(), closeBtn.bottomRight());
    painter.drawLine(closeBtn.topRight(), closeBtn.bottomLeft());

    // Separator line
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.drawLine(16, kHeaderHeight, width() - 16, kHeaderHeight);

    // Items
    if (!engine_) return;

    QFont itemFont("sans-serif", 12);
    itemFont.setStyleHint(QFont::SansSerif);
    painter.setFont(itemFont);

    int visible = (height() - kHeaderHeight) / kItemHeight;

    for (int i = 0; i < visible; i++) {
        int fileIdx = i + scrollOffset_;
        if (fileIdx >= engine_->files().size()) break;

        QRect r = itemRect(fileIdx);
        bool isCurrent = (fileIdx == currentIndex_);
        bool isHovered = (fileIdx == hoveredIndex_);

        // Highlight background
        if (isCurrent) {
            painter.fillRect(r, QColor(255, 255, 255, 20));
        } else if (isHovered) {
            painter.fillRect(r, QColor(255, 255, 255, 8));
        }

        // Number
        QColor numColor = isCurrent ? QColor(0xFF, 0xFF, 0xFF)
                                    : QColor(0x80, 0x80, 0x80);
        painter.setPen(numColor);
        QFont numFont("sans-serif", 10);
        numFont.setStyleHint(QFont::SansSerif);
        painter.setFont(numFont);
        painter.drawText(QRect(12, r.y(), 30, kItemHeight),
                         Qt::AlignVCenter | Qt::AlignRight,
                         QString::number(fileIdx + 1));

        // Filename
        QColor textColor = isCurrent ? QColor(0xFF, 0xFF, 0xFF)
                                     : QColor(0xC0, 0xC0, 0xC0);
        painter.setPen(textColor);
        painter.setFont(itemFont);

        QString name = QFileInfo(engine_->files().at(fileIdx)).fileName();
        QRect textRect(48, r.y(), width() - 60, kItemHeight);
        QString elided = painter.fontMetrics().elidedText(
            name, Qt::ElideRight, textRect.width());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
    }
}

}  // namespace vsr
