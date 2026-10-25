#include "StereoController.h"

#include "Log.h"

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScreen>
#include <QVariantMap>
#include <QImage>

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

StereoController::~StereoController() {
    destroyOsdView();
}

QScreen *StereoController::screenAt(int index) const {
    if (index < 0 || index >= screenPtrs_.size())
        return nullptr;
    return screenPtrs_[index];
}

void StereoController::refreshScreens() {
    screenPtrs_ = QGuiApplication::screens();
    screens_.clear();
    QStringList names;
    for (int i = 0; i < screenPtrs_.size(); ++i) {
        QScreen *s = screenPtrs_[i];
        QVariantMap m;
        m.insert("index", i);
        m.insert("name", s->name());
        m.insert("width", s->geometry().width());
        m.insert("height", s->geometry().height());
        m.insert("model", s->model());
        screens_.append(m);
        names << QStringLiteral("%1:%2 %3x%4")
                     .arg(i).arg(s->name())
                     .arg(s->geometry().width())
                     .arg(s->geometry().height());
        MLOG_INFO("stereo: screen %d = %s (%dx%d)", i,
                  s->name().toUtf8().constData(),
                  s->geometry().width(), s->geometry().height());
    }
    emit screensChanged();

    // 正在放映的屏幕被拔掉 → 结束会话（避免残留窗口/状态）。
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

void StereoController::createOsdView(QScreen *screen) {
    osdView_ = new QQuickView();
    osdView_->setColor(Qt::black);
    // 3D 期间不要让眼镜窗口抢焦点：主窗口保持焦点，全局快捷键（J/K/L/Tab）
    // 仍然有效；必要时也可以直接看主窗口状态。
    osdView_->setFlags(Qt::Window | Qt::FramelessWindowHint |
                       Qt::WindowDoesNotAcceptFocus);
    osdView_->setResizeMode(QQuickView::SizeRootObjectToView);
    osdView_->setScreen(screen);
    osdView_->rootContext()->setContextProperty("stereo", this);
    osdView_->setSource(QUrl("qrc:/StereoOsd.qml"));
    if (osdView_->status() != QQuickView::Ready) {
        for (const auto &e : osdView_->errors())
            MLOG_ERR("stereo: OSD QML error: %s",
                     e.toString().toUtf8().constData());
        delete osdView_;
        osdView_ = nullptr;
        return;
    }
    osdView_->showFullScreen();
    MLOG_INFO("stereo: OSD window on %s (%dx%d)",
              screen->name().toUtf8().constData(),
              screen->geometry().width(), screen->geometry().height());
}

void StereoController::destroyOsdView() {
    if (!osdView_)
        return;
    osdView_->close();
    delete osdView_;
    osdView_ = nullptr;
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
    if (mode != 1 && mode != 2)
        return;
    QScreen *s = screenAt(screenIndex);
    if (!s) {
        MLOG_WARN("stereo: start ignored, invalid screen index %d", screenIndex);
        return;
    }
    if (active_)
        destroyOsdView();

    mode_ = mode;
    layout_ = 0;
    swap_ = false;
    fit_ = false;
    osdVisible_ = true;
    screenName_ = s->name();
    active_ = true;
    // 记忆仅作为下次选择器的默认选中；不在这里/启动时自动切屏。
    preferredScreenName_ = s->name();
    settings_.setValue("stereoScreen", preferredScreenName_);
    settings_.sync();
    createOsdView(s);
    if (!osdView_) {
        active_ = false;
        mode_ = 0;
        screenName_.clear();
    }
    emit stateChanged();
    emit osdVisibleChanged();
}

void StereoController::stop() {
    if (!active_ && !osdView_)
        return;
    destroyOsdView();
    active_ = false;
    mode_ = 0;
    screenName_.clear();
    emit stateChanged();
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

void StereoController::toggleFit() {
    if (!active_)
        return;
    fit_ = !fit_;
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

bool StereoController::grabToFile(const QString &path) {
    if (!osdView_)
        return false;
    QImage img = osdView_->grabWindow();
    if (img.isNull())
        return false;
    bool ok = img.save(path);
    MLOG_INFO("stereo: grab %dx%d -> %s (ok=%d)", img.width(), img.height(),
              path.toUtf8().constData(), ok);
    return ok;
}
