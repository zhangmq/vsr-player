#pragma once

#include <QWidget>
#include <QStringList>
#include <QPropertyAnimation>

namespace vsr {

/// Small popup menu for quality selection, anchored above the quality button.
class QualityPopup : public QWidget {
    Q_OBJECT

public:
    explicit QualityPopup(QWidget* parent = nullptr);

    void setCurrentQuality(int quality);  // 0=LOW, 1=MEDIUM, 2=HIGH, 3=ULTRA

    /// Show popup anchored at screen point
    void showAt(const QPoint& anchorBottomCenter);
    void hidePopup();

signals:
    void qualitySelected(int quality);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int itemAtPos(const QPoint& pos) const;

    int currentQuality_ = 2;  // HIGH
    int hoveredIndex_ = -1;

    static const QStringList kLabels;
    static constexpr int kItemHeight = 32;
    static constexpr int kWidth = 120;
};

}  // namespace vsr
