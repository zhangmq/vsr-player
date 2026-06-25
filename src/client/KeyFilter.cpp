#include "KeyFilter.h"
#include "PlayerViewModel.h"
#include "PlaylistEngine.h"

#include <QEvent>
#include <QKeyEvent>
#include <QQuickWindow>

namespace vsr {

KeyFilter::KeyFilter(PlayerViewModel* c, PlaylistEngine* pl, QObject* p)
    : QObject(p), ctrl(c), playlist(pl) {}

bool KeyFilter::eventFilter(QObject*, QEvent* e) {
    if (e->type() != QEvent::KeyPress) return false;
    auto* ke = static_cast<QKeyEvent*>(e);
    if (ke->isAutoRepeat()) return false;

    switch (ke->key()) {
    case Qt::Key_Space:
        ctrl->togglePlayPause(); return true;
    case Qt::Key_Left:
        ctrl->seekRelative(ke->modifiers() & Qt::ShiftModifier ? -10000 : -5000);
        return true;
    case Qt::Key_Right:
        ctrl->seekRelative(ke->modifiers() & Qt::ShiftModifier ? 10000 : 5000);
        return true;
    case Qt::Key_Up:
        vol_ = ctrl->volume();
        ctrl->setVolume(vol_ + 0.05 > 1.0 ? 1.0 : vol_ + 0.05);
        vol_ = ctrl->volume();
        return true;
    case Qt::Key_Down:
        vol_ = ctrl->volume();
        ctrl->setVolume(vol_ - 0.05 < 0.0 ? 0.0 : vol_ - 0.05);
        vol_ = ctrl->volume();
        return true;
    case Qt::Key_S:
        ctrl->screenshot(); return true;
    case Qt::Key_Tab:
        ctrl->toggleOsd(); return true;
    case Qt::Key_N: {
        QString f = playlist->next();
        if (!f.isEmpty()) ctrl->loadFile(f);
        return true;
    }
    case Qt::Key_B: {
        QString f = playlist->previous();
        if (!f.isEmpty()) ctrl->loadFile(f);
        return true;
    }
    case Qt::Key_BracketLeft:
        ctrl->setSpeed(0.5); return true;
    case Qt::Key_BracketRight:
        ctrl->setSpeed(2.0); return true;
    case Qt::Key_Backslash:
        ctrl->setSpeed(1.0); return true;
    default: break;
    }
    return false;
}

}  // namespace vsr

#include "moc_KeyFilter.cpp"
