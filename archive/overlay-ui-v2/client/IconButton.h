#pragma once

#include <QWidget>

#include "IconProvider.h"

namespace vsr {

/// Pure QWidget icon button — flat design, no native widget style.
/// Renders an SVG icon via IconProvider, handles hover/click.
class IconButton : public QWidget {
    Q_OBJECT

public:
    explicit IconButton(IconName icon, int iconSize = 22,
                        QWidget* parent = nullptr);

    void setIcon(IconName icon);
    void setIconSize(int size);
    void setColor(const QColor& normal, const QColor& hover);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    IconName icon_;
    int iconSize_;
    QColor normalColor_{0xC8, 0xC8, 0xC8};
    QColor hoverColor_{0xFF, 0xFF, 0xFF};
    bool hovered_ = false;
    bool pressed_ = false;
};

}  // namespace vsr
