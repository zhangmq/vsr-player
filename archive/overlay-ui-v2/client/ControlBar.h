#pragma once

#include <QWidget>
#include <cstdint>

#include "IconProvider.h"

namespace vsr {

class ControlBar : public QWidget {
    Q_OBJECT

public:
    explicit ControlBar(QWidget* parent = nullptr);

    void setPlaying(bool playing);
    void setPosition(int64_t positionMs);
    void setDuration(int64_t durationMs);
    void setVolume(double volume);
    void setMuted(bool muted);
    void setHwDecoding(bool hw);
    void setQuality(int quality);

    void showBar();
    void hideBar();

signals:
    void previousClicked();
    void playPauseClicked();
    void nextClicked();
    void stopClicked();
    void volumeToggled();
    void hwToggled();
    void qualityClicked();
    void playlistToggled();
    void seekRequested(int64_t positionMs);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class HitTarget {
        None,
        PrevBtn, PlayBtn, NextBtn, StopBtn,
        SeekBar,
        VolumeBtn, HwBtn, QualityBtn, PlaylistBtn,
    };

    HitTarget hitTest(const QPoint& pos) const;
    QRect iconButtonRect(int index) const;
    QRect rightButtonRect(int index) const;
    QRect seekBarRect() const;

    void drawSeekBar(QPainter& p);
    void drawTimeLabel(QPainter& p);

    bool playing_ = false;
    int64_t positionMs_ = 0;
    int64_t durationMs_ = 0;
    double volume_ = 1.0;
    bool muted_ = false;
    bool hwDecoding_ = true;
    int quality_ = 2;

    HitTarget hovered_ = HitTarget::None;
    HitTarget pressed_ = HitTarget::None;
    bool seekDragging_ = false;
    double seekDragFraction_ = 0.0;

    static constexpr int kHeight = 52;
    static constexpr int kHMargin = 12;
    static constexpr int kIconSize = 20;
    static constexpr int kButtonSpacing = 6;
};

}  // namespace vsr
