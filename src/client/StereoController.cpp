#include "StereoController.h"

#include "Log.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QVariantMap>

StereoController::StereoController(QObject *parent) : QObject(parent) {
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
        connect(s, &QScreen::geometryChanged, this,
                &StereoController::refreshScreens, Qt::UniqueConnection);
        refreshScreens();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this,
            &StereoController::refreshScreens);
    for (QScreen *s : QGuiApplication::screens())
        connect(s, &QScreen::geometryChanged, this,
                &StereoController::refreshScreens, Qt::UniqueConnection);
    preferredScreenName_ = settings_.value("stereoScreen").toString();
    refreshScreens();
}

StereoController::~StereoController() = default;

QScreen *StereoController::screenAt(int index) const {
    if (index < 0 || index >= screenPtrs_.size())
        return nullptr;
    return screenPtrs_[index];
}

void StereoController::refreshScreens() {
    screenPtrs_ = QGuiApplication::screens();
    screens_.clear();
    for (int i = 0; i < screenPtrs_.size(); ++i) {
        QScreen *s = screenPtrs_[i];
        QVariantMap m;
        m.insert("index", i);
        m.insert("name", s->name());
        m.insert("width", s->geometry().width());
        m.insert("height", s->geometry().height());
        m.insert("model", s->model());
        screens_.append(m);
        MLOG_INFO("stereo: screen %d = %s (%dx%d)", i,
                  s->name().toUtf8().constData(),
                  s->geometry().width(), s->geometry().height());
    }
    emit screensChanged();

    // 目标屏被拔掉 → 结束会话并清理窗口；不做“切回 2D 自动结束”。
    if (active_) {
        bool found = false;
        for (QScreen *s : screenPtrs_)
            if (s->name() == screenName_)
                found = true;
        if (!found) {
            MLOG_WARN("stereo: active screen '%s' disappeared -> stop",
                      screenName_.toUtf8().constData());
            stop();
        }
    }
}

int StereoController::preferredScreenIndex() const {
    if (preferredScreenName_.isEmpty())
        return -1;
    for (int i = 0; i < screenPtrs_.size(); ++i)
        if (screenPtrs_[i]->name() == preferredScreenName_)
            return i;
    return -1;
}

void StereoController::start(int mode, int screenIndex) {
    // 2D→3D 本期只占位：不创建 presentation、不伪造 3D。
    if (mode != 1)
        return;
    QScreen *s = screenAt(screenIndex);
    if (!s) {
        MLOG_WARN("stereo: start ignored, invalid screen index %d", screenIndex);
        return;
    }
    if (active_)
        stop();

    mode_ = mode;
    layout_ = 0;
    swap_ = false;
    half_ = false;
    osdVisible_ = true;
    screenName_ = s->name();
    active_ = true;

    if (playerWin_) {
        origScreen_ = playerWin_->screen();
        origGeom_ = playerWin_->geometry();
        origFullScreen_ = (playerWin_->visibility() == QWindow::FullScreen);
        origFlags_ = playerWin_->flags();
        origCursor_ = playerWin_->cursor();
    }

    // 桌面 control 窗口：沿用主窗口进入 3D 前的屏幕/几何，让主 UI 看起来没“消失”。
    if (controlWin_) {
        if (origScreen_)
            controlWin_->setScreen(origScreen_);
        if (origFullScreen_) {
            controlWin_->showFullScreen();
        } else {
            controlWin_->showNormal();
            if (origGeom_.isValid())
                controlWin_->setGeometry(origGeom_);
        }
        controlWin_->requestActivate();
    }

    // presentation：主窗口（Video/VO）移到眼镜屏，满屏、置顶、无边框、不收焦点。
    if (playerWin_) {
        playerWin_->setFlags(origFlags_ | Qt::FramelessWindowHint |
                             Qt::WindowStaysOnTopHint |
                             Qt::WindowDoesNotAcceptFocus);
        playerWin_->setCursor(Qt::BlankCursor);
        playerWin_->setScreen(s);
        playerWin_->showFullScreen();
    }

    preferredScreenName_ = s->name();
    settings_.setValue("stereoScreen", preferredScreenName_);
    settings_.sync();

    emit stateChanged();
    emit osdVisibleChanged();
    MLOG_INFO("stereo: presentation on %s (%dx%d)", s->name().toUtf8().constData(),
              s->geometry().width(), s->geometry().height());
}

void StereoController::stop() {
    if (!active_)
        return;

    if (controlWin_)
        controlWin_->hide();

    if (playerWin_) {
        playerWin_->setFlags(origFlags_);
        playerWin_->setCursor(origCursor_);
        if (origScreen_)
            playerWin_->setScreen(origScreen_);
        if (origFullScreen_) {
            playerWin_->showFullScreen();
        } else {
            playerWin_->showNormal();
            if (origGeom_.isValid())
                playerWin_->setGeometry(origGeom_);
        }
    }

    active_ = false;
    mode_ = 0;
    screenName_.clear();
    emit stateChanged();
    MLOG_INFO("stereo: stopped");
}

void StereoController::cycleLayout() {
    if (!active_)
        return;
    layout_ = layout_ == 0 ? 1 : 0;
    emit stateChanged();
}

void StereoController::swapEyes() {
    if (!active_)
        return;
    swap_ = !swap_;
    emit stateChanged();
}

void StereoController::toggleHalf() {
    if (!active_)
        return;
    half_ = !half_;
    emit stateChanged();
}

void StereoController::toggleOsd() {
    if (!active_)
        return;
    setOsdVisible(!osdVisible_);
}

void StereoController::setOsdVisible(bool v) {
    if (osdVisible_ == v)
        return;
    osdVisible_ = v;
    emit osdVisibleChanged();
}
