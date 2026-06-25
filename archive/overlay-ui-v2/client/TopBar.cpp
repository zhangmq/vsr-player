#include "TopBar.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

namespace vsr {

TopBar::TopBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
}

void TopBar::setFileName(const QString& name) {
    fileName_ = name;
    update();
}

void TopBar::showBar() {
    show();
    raise();
}

void TopBar::hideBar() {
    hide();
}

void TopBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (parentWidget()) {
        setGeometry(kHMargin, 8,
                    parentWidget()->width() - kHMargin * 2, kHeight);
    }
}

void TopBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Background
    QPainterPath path;
    path.addRoundedRect(rect(), 6, 6);
    painter.fillPath(path, QColor(0, 0, 0, 180));

    // File name
    painter.setPen(QColor(0xE0, 0xE0, 0xE0));
    QFont font("sans-serif", 13);
    font.setStyleHint(QFont::SansSerif);
    painter.setFont(font);

    QRect textRect = rect().adjusted(12, 0, -12, 0);
    QString elided = painter.fontMetrics().elidedText(
        fileName_, Qt::ElideMiddle, textRect.width());
    painter.drawText(textRect, Qt::AlignCenter, elided);
}

}  // namespace vsr
