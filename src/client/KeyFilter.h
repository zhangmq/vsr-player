#pragma once

#include <QObject>

class PlaylistEngine;

namespace vsr {

class PlayerViewModel;

/// Keyboard shortcut handler.
/// Installed as an event filter on the QQuickView.
class KeyFilter : public QObject {
    Q_OBJECT

public:
    KeyFilter(PlayerViewModel* ctrl, PlaylistEngine* playlist, QObject* parent);

    bool eventFilter(QObject* watched, QEvent* event) override;

    double vol_ = 1.0;

signals:
    void togglePlaylist();

private:
    PlayerViewModel* ctrl;
    PlaylistEngine* playlist;
};

}  // namespace vsr
