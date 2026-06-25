#include "ControlBar.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QMouseEvent>
#include <cmath>

namespace vsr {

ControlBar::ControlBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setMouseTracking(true);
    hide();
}

void ControlBar::setPlaying(bool playing) {
    playing_ = playing;
    update();
}

void ControlBar::setPosition(int64_t positionMs) {
    positionMs_ = positionMs;
    if (!seekDragging_) update();
}

void ControlBar::setDuration(int64_t durationMs) {
    durationMs_ = durationMs;
    update();
}

void ControlBar::setVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    update();
}

void ControlBar::setMuted(bool muted) {
    muted_ = muted;
    update();
}

void ControlBar::setHwDecoding(bool hw) {
    hwDecoding_ = hw;
    update();
}

void ControlBar::setQuality(int quality) {
    quality_ = quality;
    update();
}

void ControlBar::showBar() {
    show();
    raise();
}

void ControlBar::hideBar() {
    hide();
}

// ── Layout helpers ─────────────────────────────────────────────────────

QRect ControlBar::iconButtonRect(int index) const {
    int x = kHMargin + index * (kIconSize + kButtonSpacing);
    int y = (height() - kIconSize) / 2;
    return QRect(x, y, kIconSize, kIconSize);
}

QRect ControlBar::rightButtonRect(int index) const {
    int x = width() - kHMargin - (4 - index) * (kIconSize + kButtonSpacing);
    int y = (height() - kIconSize) / 2;
    return QRect(x, y, kIconSize, kIconSize);
}

QRect ControlBar::seekBarRect() const {
    int left = kHMargin + 4 * (kIconSize + kButtonSpacing) + 20;
    int right = width() - kHMargin - 4 * (kIconSize + kButtonSpacing) - 20;
    int barH = 4;
    int y = (height() - barH) / 2;
    return QRect(left, y - 8, right - left, 16);
}

// ── Hit testing ────────────────────────────────────────────────────────

ControlBar::HitTarget ControlBar::hitTest(const QPoint& pos) const {
    for (int i = 0; i < 4; i++) {
        QRect r = iconButtonRect(i);
        if (r.adjusted(-4, -4, 4, 4).contains(pos)) {
            switch (i) {
            case 0: return HitTarget::PrevBtn;
            case 1: return HitTarget::PlayBtn;
            case 2: return HitTarget::NextBtn;
            case 3: return HitTarget::StopBtn;
            default: break;
            }
        }
    }
    if (seekBarRect().contains(pos))
        return HitTarget::SeekBar;
    for (int i = 0; i < 4; i++) {
        QRect r = rightButtonRect(i);
        if (r.adjusted(-4, -4, 4, 4).contains(pos)) {
            switch (i) {
            case 0: return HitTarget::VolumeBtn;
            case 1: return HitTarget::HwBtn;
            case 2: return HitTarget::QualityBtn;
            case 3: return HitTarget::PlaylistBtn;
            default: break;
            }
        }
    }
    return HitTarget::None;
}

// ── Mouse events ───────────────────────────────────────────────────────

void ControlBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    pressed_ = hitTest(event->pos());
    if (pressed_ == HitTarget::SeekBar) {
        seekDragging_ = true;
        QRect sr = seekBarRect();
        double frac = double(event->pos().x() - sr.x()) / sr.width();
        seekDragFraction_ = std::clamp(frac, 0.0, 1.0);
        update();
    }
}

void ControlBar::mouseMoveEvent(QMouseEvent* event) {
    if (seekDragging_) {
        QRect sr = seekBarRect();
        double frac = double(event->pos().x() - sr.x()) / sr.width();
        seekDragFraction_ = std::clamp(frac, 0.0, 1.0);
        update();
        return;
    }
    HitTarget prev = hovered_;
    hovered_ = hitTest(event->pos());
    if (prev != hovered_) {
        setCursor(hovered_ == HitTarget::None ? Qt::ArrowCursor
                                              : Qt::PointingHandCursor);
        update();
    }
}

void ControlBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (seekDragging_) {
        seekDragging_ = false;
        if (durationMs_ > 0) {
            int64_t pos = int64_t(seekDragFraction_ * durationMs_);
            emit seekRequested(pos);
        }
        update();
        pressed_ = HitTarget::None;
        return;
    }
    HitTarget hit = hitTest(event->pos());
    if (hit == pressed_ && hit != HitTarget::None) {
        switch (hit) {
        case HitTarget::PrevBtn:     emit previousClicked(); break;
        case HitTarget::PlayBtn:     emit playPauseClicked(); break;
        case HitTarget::NextBtn:     emit nextClicked(); break;
        case HitTarget::StopBtn:     emit stopClicked(); break;
        case HitTarget::VolumeBtn:   emit volumeToggled(); break;
        case HitTarget::HwBtn:       emit hwToggled(); break;
        case HitTarget::QualityBtn:  emit qualityClicked(); break;
        case HitTarget::PlaylistBtn: emit playlistToggled(); break;
        default: break;
        }
    }
    pressed_ = HitTarget::None;
    update();
}

// ── Resize ─────────────────────────────────────────────────────────────

void ControlBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (parentWidget()) {
        setGeometry(kHMargin,
                    parentWidget()->height() - kHeight - 8,
                    parentWidget()->width() - kHMargin * 2,
                    kHeight);
    }
}

// ── Paint ──────────────────────────────────────────────────────────────

void ControlBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Background
    QPainterPath bgPath;
    bgPath.addRoundedRect(rect(), 8, 8);
    painter.fillPath(bgPath, QColor(0, 0, 0, 190));

    // Left buttons: prev, play, next, stop
    static const IconName leftIcons[] = {
        IconName::SkipBack, IconName::Play, IconName::SkipForward, IconName::Stop
    };
    for (int i = 0; i < 4; i++) {
        IconName icon = leftIcons[i];
        if (i == 1) icon = playing_ ? IconName::Pause : IconName::Play;
        QRect r = iconButtonRect(i);
        bool isHovered = (hovered_ == static_cast<HitTarget>(
            static_cast<int>(HitTarget::PrevBtn) + i));
        bool isPressed = (pressed_ == static_cast<HitTarget>(
            static_cast<int>(HitTarget::PrevBtn) + i));
        QColor c = isPressed ? QColor(0xCC, 0xCC, 0xCC)
                 : isHovered ? QColor(0xFF, 0xFF, 0xFF)
                 : QColor(0xC8, 0xC8, 0xC8);
        QPixmap pm = IconProvider::pixmap(icon, kIconSize, c);
        painter.drawPixmap(r.x(), r.y(), pm);
    }

    // Seek bar
    drawSeekBar(painter);

    // Time label
    drawTimeLabel(painter);

    // Right buttons: volume, hw, quality, playlist
    for (int i = 0; i < 4; i++) {
        QRect r = rightButtonRect(i);
        bool isHovered = (hovered_ == static_cast<HitTarget>(
            static_cast<int>(HitTarget::VolumeBtn) + i));
        bool isPressed = (pressed_ == static_cast<HitTarget>(
            static_cast<int>(HitTarget::VolumeBtn) + i));

        if (i == 1) {
            // HW/SW toggle
            QString label = hwDecoding_ ? "HW" : "SW";
            QColor textColor = isHovered ? QColor(0xFF, 0xFF, 0xFF)
                             : hwDecoding_ ? QColor(0x80, 0xD0, 0x80)
                             : QColor(0xD0, 0x80, 0x80);
            QFont f("sans-serif", 9);
            f.setStyleHint(QFont::SansSerif);
            f.setBold(true);
            painter.setFont(f);
            painter.setPen(textColor);
            painter.drawText(r, Qt::AlignCenter, label);
        } else {
            IconName icon;
            switch (i) {
            case 0: icon = (muted_ || volume_ == 0.0) ? IconName::VolumeMuted
                                                       : IconName::Volume; break;
            case 2: icon = IconName::Settings; break;
            case 3: icon = IconName::Playlist; break;
            default: continue;
            }
            QColor c = isPressed ? QColor(0xCC, 0xCC, 0xCC)
                     : isHovered ? QColor(0xFF, 0xFF, 0xFF)
                     : QColor(0xC8, 0xC8, 0xC8);
            QPixmap pm = IconProvider::pixmap(icon, kIconSize, c);
            painter.drawPixmap(r.x(), r.y(), pm);
        }
    }
}

void ControlBar::drawSeekBar(QPainter& p) {
    QRect sr = seekBarRect();
    int barH = 4;
    int barY = (height() - barH) / 2;

    // Track background
    QPainterPath track;
    track.addRoundedRect(sr.x(), barY, sr.width(), barH, 2, 2);
    p.fillPath(track, QColor(255, 255, 255, 40));

    // Filled portion
    double frac = seekDragging_ ? seekDragFraction_
                  : (durationMs_ > 0 ? double(positionMs_) / durationMs_ : 0.0);
    frac = std::clamp(frac, 0.0, 1.0);
    int fillW = int(sr.width() * frac);

    if (fillW > 0) {
        QPainterPath fill;
        fill.addRoundedRect(sr.x(), barY, fillW, barH, 2, 2);
        p.fillPath(fill, QColor(0xE0, 0xE0, 0xE0));
    }

    // Handle dot
    int handleX = sr.x() + fillW;
    int handleY = height() / 2;
    if (seekDragging_ || hovered_ == HitTarget::SeekBar) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xFF, 0xFF, 0xFF));
        p.drawEllipse(QPoint(handleX, handleY), 5, 5);
    }
}

void ControlBar::drawTimeLabel(QPainter& p) {
    auto fmtTime = [](int64_t ms) -> QString {
        if (ms < 0) ms = 0;
        int sec = (ms / 1000) % 60;
        int min = ms / 60000;
        return QString("%1:%2").arg(min, 1, 10, QChar('0'))
                               .arg(sec, 2, 10, QChar('0'));
    };

    int64_t pos = seekDragging_ ? int64_t(seekDragFraction_ * durationMs_)
                                : positionMs_;
    QString text = fmtTime(pos) + " / " + fmtTime(durationMs_);

    QRect sr = seekBarRect();
    int labelW = 140;
    int labelX = sr.right() + 12;

    p.setPen(QColor(0xC0, 0xC0, 0xC0));
    QFont font("sans-serif", 11);
    font.setStyleHint(QFont::SansSerif);
    p.setFont(font);
    p.drawText(QRect(labelX, 0, labelW, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
}

}  // namespace vsr
