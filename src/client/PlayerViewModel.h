#pragma once

#include <QObject>
#include <QString>

#include <cstdint>

namespace vsr {

class Player;
enum class Quality : int;
struct PlayerEvent;

/// Single source of truth for all UI state.
/// QML binds to Q_PROPERTYs; user actions call slots.
/// Slots optimistically update properties before sending commands to core.
/// Core events update properties with authoritative values.
class PlayerViewModel : public QObject {
    Q_OBJECT
    // Playback
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(int64_t currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    // Overlay
    Q_PROPERTY(bool overlaysVisible READ overlaysVisible WRITE setOverlaysVisible NOTIFY overlaysVisibleChanged)
    // Audio
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    // Video settings
    Q_PROPERTY(int quality READ quality NOTIFY qualityChanged)
    Q_PROPERTY(bool vsrActive READ vsrActive NOTIFY vsrActiveChanged)
    Q_PROPERTY(int scale READ scale NOTIFY scaleChanged)
    Q_PROPERTY(int denoiseQuality READ denoiseQuality NOTIFY denoiseQualityChanged)
    // Speed
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    // Info
    Q_PROPERTY(bool hwDecoding READ hwDecoding NOTIFY hwDecodingChanged)
    Q_PROPERTY(QString videoInfo READ videoInfo NOTIFY videoInfoChanged)
    // Window
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)

public:
    bool playing() const        { return playing_; }
    int64_t currentTime() const { return currentTime_; }
    int64_t duration() const    { return duration_; }
    bool overlaysVisible() const { return overlaysVisible_; }
    double volume() const       { return volume_; }
    bool muted() const          { return volume_ < 0.01; }
    int quality() const         { return quality_; }
    bool vsrActive() const      { return scale_ != -1 || denoise_quality_ != -1; }
    int scale() const           { return scale_; }
    int denoiseQuality() const  { return denoise_quality_; }
    double speed() const        { return speed_; }
    bool hwDecoding() const     { return hwDecoding_; }
    QString videoInfo() const   { return videoInfo_; }
    bool fullscreen() const     { return fullscreen_; }

    void setOverlaysVisible(bool v);
    void setFullscreen(bool fs);

    void setPlayer(Player* p) { player_ = p; }
    Player* player() const { return player_; }

public slots:
    void togglePlayPause();
    void stop();
    void seekAbsolute(int64_t ms);
    void seekRelative(int64_t offsetMs);
    void setVolume(double vol);
    void setQuality(int q);
    void toggleMute();
    void toggleHwaccel();
    void screenshot();
    void loadFile(const QString& path);
    void setSpeed(double speed);
    void setScale(int s);
    void setDenoiseQuality(int d);
    void toggleFullscreen();

    // Called from event callback (via QueuedConnection)
    void updateState(bool playing);
    void updateTime(int64_t t, int64_t d);
    void updateVideoInfo(const QString& info);
    void updateVsrActive(bool active);
    void updateHwDecoding(bool hw);
    void updateQuality(int q);
    void updateScale(int s);
signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void videoInfoChanged();
    void vsrActiveChanged();
    void hwDecodingChanged();
    void speedChanged();
    void volumeChanged();
    void mutedChanged();
    void qualityChanged();
    void scaleChanged();
    void denoiseQualityChanged();
    void overlaysVisibleChanged();
    void fullscreenChanged();

private:
    Player* player_ = nullptr;
    bool playing_ = false;
    double last_good_volume_ = 0.65;  // last volume ≥ 0.01, for unmute restore
    int64_t currentTime_ = 0;
    int64_t duration_ = 0;
    QString videoInfo_;
    bool hwDecoding_ = false;
    double speed_ = 1.0;
    double volume_ = 0.65;
    int quality_ = 3;
    int denoise_quality_ = -1;
    int scale_ = 0;
    bool overlaysVisible_ = true;
    bool fullscreen_ = false;
};

}  // namespace vsr
