#include "StereoController.h"

#include "Log.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

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

// Wayland 下 QScreen::geometry 是**逻辑**尺寸（高 DPI/分数缩放后），
// 不是物理输出模式；例如 3840×1080 的眼镜可能报 2844×800。
// KWin 的 kscreen-doctor -j 有每个输出当前模式的物理像素尺寸；这里只用于
// 选择器显示/识别，不参与渲染。没有该工具（非 KDE）时退回 QScreen 尺寸。
static QHash<QString, QSize> physicalModes()
{
    QHash<QString, QSize> out;
    const QString exe = QStandardPaths::findExecutable("kscreen-doctor");
    if (exe.isEmpty())
        return out;
    QProcess p;
    p.start(exe, {QStringLiteral("-j")});
    if (!p.waitForFinished(800))
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
    for (const QJsonValue &v : doc.object().value("outputs").toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value("name").toString();
        const QString cur = o.value("currentModeId").toString();
        for (const QJsonValue &mv : o.value("modes").toArray()) {
            const QJsonObject m = mv.toObject();
            if (m.value("id").toString() != cur)
                continue;
            const QJsonObject sz = m.value("size").toObject();
            out.insert(name, QSize(sz.value("width").toInt(),
                                   sz.value("height").toInt()));
            break;
        }
    }
    return out;
}

void StereoController::refreshScreens() {
    const QHash<QString, QSize> phys = physicalModes();
    screenPtrs_ = QGuiApplication::screens();
    screens_.clear();
    for (int i = 0; i < screenPtrs_.size(); ++i) {
        QScreen *s = screenPtrs_[i];
        const QRect g = s->geometry();
        const QSize physical = phys.value(s->name(), g.size());
        QVariantMap m;
        m.insert("index", i);
        m.insert("name", s->name());
        m.insert("model", s->model());
        m.insert("manufacturer", s->manufacturer());
        m.insert("width", physical.width());
        m.insert("height", physical.height());
        m.insert("logicalWidth", g.width());
        m.insert("logicalHeight", g.height());
        screens_.append(m);
        MLOG_INFO("stereo: screen %d = %s model=%s physical=%dx%d logical=%dx%d",
                  i, s->name().toUtf8().constData(),
                  s->model().toUtf8().constData(),
                  physical.width(), physical.height(), g.width(), g.height());
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

    // 桌面 control 窗口：优先沿用主 UI 进入 3D 前的屏幕；但如果主 UI 当时
    // 就在目标（眼镜）屏上，control 必须换到 primary/其他非目标屏，否则
    // 右键菜单/UI 还是会出现在眼镜上。
    if (controlWin_) {
        QScreen *controlScreen = origScreen_;
        if (!controlScreen || controlScreen == s) {
            controlScreen = QGuiApplication::primaryScreen();
            if (!controlScreen || controlScreen == s) {
                for (QScreen *cand : QGuiApplication::screens()) {
                    if (cand != s) { controlScreen = cand; break; }
                }
            }
        }
        if (controlScreen) {
            // Wayland 初始放置看 active output，show() 之前 setScreen 不生效；
            // 先 show 让平台窗口存在，再 setScreen/geometry。
            controlWin_->setOpacity(0.0);
            controlWin_->show();
            controlWin_->setScreen(controlScreen);
            if (origFullScreen_) {
                controlWin_->showFullScreen();
            } else {
                controlWin_->showNormal();
                if (origGeom_.isValid())
                    controlWin_->setGeometry(origGeom_);
            }
            controlWin_->setOpacity(1.0);
        }
        controlWin_->requestActivate();
        MLOG_INFO("stereo: control window on %s geom=%dx%d+%d+%d (orig screen %s)",
                  controlWin_->screen() ? controlWin_->screen()->name().toUtf8().constData() : "?",
                  controlWin_->geometry().width(), controlWin_->geometry().height(),
                  controlWin_->geometry().x(), controlWin_->geometry().y(),
                  origScreen_ ? origScreen_->name().toUtf8().constData() : "?");
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
    const QVariantMap sm = screens_.value(screenIndex).toMap();
    MLOG_INFO("stereo: presentation on %s (physical %dx%d, logical %dx%d)",
              s->name().toUtf8().constData(),
              sm.value("width").toInt(), sm.value("height").toInt(),
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
