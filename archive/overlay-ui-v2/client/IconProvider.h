#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>

namespace vsr {

enum class IconName {
    SkipBack,
    Play,
    Pause,
    SkipForward,
    Stop,
    Volume,
    VolumeMuted,
    Settings,
    Playlist,
    Close,
};

class IconProvider {
public:
    static QPixmap pixmap(IconName name, int size,
                          const QColor& color = QColor(0xC8, 0xC8, 0xC8));
    static QString pathData(IconName name);

private:
    IconProvider() = delete;
};

}  // namespace vsr
