#include "PlayerViewModel.h"
#include "api/Player.h"

namespace vsr {

// ── Overlay visibility ───────────────────────────────────────────────

void PlayerViewModel::setOverlaysVisible(bool v) {
    if (overlaysVisible_ != v) {
        overlaysVisible_ = v;
        emit overlaysVisibleChanged();
    }
}

void PlayerViewModel::setFullscreen(bool fs) {
    if (fullscreen_ != fs) {
        fullscreen_ = fs;
        emit fullscreenChanged();
    }
}

// ── Playback ─────────────────────────────────────────────────────────

void PlayerViewModel::togglePlayPause() {
    if (!player_) return;
    playing_ = !playing_;
    emit playingChanged();
    player_->send_command(playing_ ? PlayerCommand(CmdPlay{}) : PlayerCommand(CmdPause{}));
}

void PlayerViewModel::stop() {
    if (!player_) return;
    playing_ = false;
    emit playingChanged();
    player_->send_command(CmdStop{});
}

void PlayerViewModel::seekAbsolute(int64_t ms) {
    if (!player_) return;
    player_->send_command(CmdSeek{ms});
}

void PlayerViewModel::seekRelative(int64_t offsetMs) {
    if (!player_) return;
    int64_t target = currentTime_ + offsetMs;
    if (target < 0) target = 0;
    if (duration_ > 0 && target > duration_) target = duration_;
    player_->send_command(CmdSeek{target});
}

// ── Audio ────────────────────────────────────────────────────────────

void PlayerViewModel::setVolume(double vol) {
    if (!player_) return;
    if (vol < 0.0) vol = 0.0;
    if (vol > 1.0) vol = 1.0;
    bool wasMuted = (volume_ < 0.01);
    if (volume_ != vol) { volume_ = vol; emit volumeChanged(); }
    // Track last meaningful volume for mute/unmute
    if (vol >= 0.01) last_good_volume_ = vol;
    bool nowMuted = (vol < 0.01);
    if (wasMuted != nowMuted) emit mutedChanged();
    player_->send_command(CmdSetVolume{vol});
}

void PlayerViewModel::toggleMute() {
    if (!player_) return;
    if (volume_ > 0.0) {
        // Mute: save current volume, set to 0
        last_good_volume_ = volume_;
        volume_ = 0.0;
    } else {
        // Unmute: restore last good volume
        volume_ = (last_good_volume_ >= 0.01) ? last_good_volume_ : 0.65;
    }
    emit volumeChanged();
    emit mutedChanged();
    player_->send_command(CmdSetVolume{volume_});
}

void PlayerViewModel::toggleHwaccel() {
    if (!player_) return;
    hwDecoding_ = !hwDecoding_;
    emit hwDecodingChanged();
    player_->send_command(CmdSetHwaccel{hwDecoding_});
}

// ── Video settings ───────────────────────────────────────────────────

void PlayerViewModel::setQuality(int q) {
    if (!player_) return;
    if (q < 1 || q > 4) return;
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
    player_->send_command(CmdSetQuality{q});
}

void PlayerViewModel::setScale(int s) {
    if (!player_) return;
    if (s < -1 || s > 4 || s == 1) return;
    if (scale_ != s) { scale_ = s; emit scaleChanged(); }
    bool wasActive = (scale_ != -1 || denoise_quality_ != -1);
    player_->send_command(CmdSetScale{s});
    bool nowActive = (s != -1 || denoise_quality_ != -1);
    if (wasActive != nowActive) emit vsrActiveChanged();
}

void PlayerViewModel::setDenoiseQuality(int d) {
    if (!player_) return;
    if (d != -1 && (d < 8 || d > 11)) return;
    if (denoise_quality_ != d) { denoise_quality_ = d; emit denoiseQualityChanged(); }
    bool wasActive = (scale_ != -1 || denoise_quality_ != -1);
    player_->send_command(CmdSetDenoiseQuality{d});
    bool nowActive = (scale_ != -1 || d != -1);
    if (wasActive != nowActive) emit vsrActiveChanged();
}

void PlayerViewModel::setSpeed(double speed) {
    if (!player_) return;
    if (speed < 0.1) speed = 0.1;
    if (speed > 4.0) speed = 4.0;
    if (speed_ != speed) { speed_ = speed; emit speedChanged(); }
    player_->send_command(CmdSetSpeed{speed});
}

void PlayerViewModel::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    emit fullscreenChanged();
}

// ── File ─────────────────────────────────────────────────────────────

void PlayerViewModel::loadFile(const QString& path) {
    if (!player_) return;
    videoInfo_ = path.split('/').last();  // filename for title bar
    emit videoInfoChanged();
    player_->send_command(CmdStop{});
    player_->send_command(CmdLoadFile{path.toStdString()});
}

void PlayerViewModel::screenshot() {
    if (player_) player_->send_command(CmdCapture{});
}

// ── Event handlers (called from main.cpp via QueuedConnection) ────────

void PlayerViewModel::updateState(bool p) {
    if (playing_ != p) { playing_ = p; emit playingChanged(); }
}

void PlayerViewModel::updateTime(int64_t t, int64_t d) {
    currentTime_ = t;
    if (d != duration_) { duration_ = d; emit durationChanged(); }
    emit currentTimeChanged();
}

void PlayerViewModel::updateVideoInfo(const QString& info) {
    videoInfo_ = info; emit videoInfoChanged();
}

void PlayerViewModel::updateVsrActive(bool /*a*/) {
    emit vsrActiveChanged();
}

void PlayerViewModel::updateHwDecoding(bool h) {
    if (hwDecoding_ != h) { hwDecoding_ = h; emit hwDecodingChanged(); }
}

void PlayerViewModel::updateQuality(int q) {
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
}

void PlayerViewModel::updateScale(int s) {
    if (scale_ != s) { scale_ = s; emit scaleChanged(); }
}

void PlayerViewModel::toggleOsd() {
    osdVisible_ = !osdVisible_;
    emit osdVisibleChanged();
}

static QString fmtTime(int64_t ms) {
    int64_t s = ms / 1000;
    return QString("%1:%2:%3")
        .arg(s / 3600,       2, 10, QChar('0'))
        .arg((s % 3600) / 60, 2, 10, QChar('0'))
        .arg(s % 60,         2, 10, QChar('0'));
}

static const char* qualityName(int v) {
    switch (v) {
    case 1:  return "Low";
    case 2:  return "Medium";
    case 3:  return "High";
    case 4:  return "Ultra";
    default: return "off";
    }
}

static const char* denoiseName(int v) {
    switch (v) {
    case 8:  return "Low";
    case 9:  return "Medium";
    case 10: return "High";
    case 11: return "Ultra";
    default: return "off";
    }
}

void PlayerViewModel::updateOsdInfo(const PlayerEvent& e) {
    QStringList lines;

    {
        QString codec = e.codec_name.empty() ? "" : e.codec_name.c_str();
        lines.append(QString("Source    %1×%2 %3 %4fps")
            .arg(e.in_width).arg(e.in_height)
            .arg(codec)
            .arg(e.fps, 0, 'f', 2));
    }

    lines.append(QString("Output    %1×%2 (%3×)")
        .arg(e.out_width).arg(e.out_height).arg(e.scale));

    {
        QString vsr = "VSR       ";
        if (e.vsr_active && e.scale > 1) {
            vsr += QString("%1 quality").arg(qualityName(e.quality));
        } else if (e.vsr_active && e.scale == 1 && e.denoise != -1) {
            vsr += QString("Denoise %1").arg(denoiseName(e.denoise));
        } else {
            vsr += "off";
        }
        lines.append(vsr);
    }

    {
        QString pix = e.pix_fmt_name.empty() ? "" : e.pix_fmt_name.c_str();
        lines.append(QString("Decoder   %1 %2")
            .arg(e.hw_decoding ? "NVDEC" : "Software")
            .arg(pix));
    }

    lines.append(QString("Speed     %1×").arg(e.speed, 0, 'f', 2));

    lines.append(QString("PTS       %1 / %2")
        .arg(fmtTime(e.time_ms)).arg(fmtTime(e.duration_ms)));

    lines.append(QString("Render    %1×%2 RGBA → %3×%4 window  %5fps")
        .arg(e.out_width).arg(e.out_height)
        .arg(e.phys_w).arg(e.phys_h)
        .arg(e.render_fps, 0, 'f', 1));

    lines.append(QString("Frames    decoded %1  dropped %2")
        .arg(e.decoded_frames).arg(e.dropped_frames));

    if (!e.gpu_name.empty()) {
        lines.append(QString("GPU       %1  %2/%3 MB")
            .arg(e.gpu_name.c_str())
            .arg(e.vram_used_mb).arg(e.vram_total_mb));
    }

    if (e.has_audio) {
        lines.append(QString("Audio     %1Hz %2ch (PortAudio)")
            .arg(e.audio_sr).arg(e.audio_ch));
    }

    osdText_ = lines.join('\n');
    emit osdTextChanged();
}

}  // namespace vsr

#include "moc_PlayerViewModel.cpp"
