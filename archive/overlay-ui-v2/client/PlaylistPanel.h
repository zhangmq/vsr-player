#pragma once

#include <QWidget>
#include <QStringList>
#include <QPropertyAnimation>

#include "PlaylistEngine.h"

namespace vsr {

/// Right-side slide-out playlist panel — custom painted, numbered items.
class PlaylistPanel : public QWidget {
    Q_OBJECT

public:
    explicit PlaylistPanel(QWidget* parent = nullptr);

    void setEngine(PlaylistEngine* engine);
    void setCurrentIndex(int index);

    /// Slide in/out
    void slideIn();
    void slideOut();
    bool isOpen() const { return open_; }

signals:
    void fileSelected(const QString& path, int index);
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    int itemAtPos(const QPoint& pos) const;
    QRect itemRect(int index) const;

    PlaylistEngine* engine_ = nullptr;
    int currentIndex_ = -1;
    int hoveredIndex_ = -1;
    int scrollOffset_ = 0;

    bool open_ = false;
    QPropertyAnimation* slideAnim_ = nullptr;

    static constexpr int kWidth = 300;
    static constexpr int kItemHeight = 32;
    static constexpr int kHeaderHeight = 40;
    static constexpr int kCloseBtnSize = 20;
};

}  // namespace vsr
