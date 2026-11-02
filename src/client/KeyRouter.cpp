#include "KeyRouter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QWindow>

#include "Log.h"
#include "PlayerViewModel.h"
#include "PopupRegistry.h"
#include "StereoController.h"

KeyRouter::KeyRouter(PlayerViewModel *vm, StereoController *stereo,
                     PopupRegistry *popups, QObject *parent)
    : QObject(parent), vm_(vm), stereo_(stereo), popups_(popups)
{
    // 启动自检：重复按键 / 空 id / 无法解析的序列在**启动时**就报出来，
    // 而不是等用户按到那个键才发现（改版前这些只能靠人肉对比三个文件）。
    const QStringList errs = keymap::selfCheck();
    for (const QString &e : errs)
        MLOG_ERR("keymap: %s", qUtf8Printable(e));
    MLOG_INFO("keymap: %d bindings, self-check %s",
              int(keymap::bindings().size()),
              errs.isEmpty() ? "OK" : "FAILED");
}

bool KeyRouter::mode3d() const
{
    // 单眼 2D 输出也走 3D 那一套键（改版前的过滤器就是这么判的：3D 或单眼态
    // 都吞掉方向键/Space，否则会同时打到窗口级动作上）。
    return (stereo_ && stereo_->active()) || (vm_ && vm_->eyeMode() != 0);
}

bool KeyRouter::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (event->type() != QEvent::KeyPress)
        return false;
    // 弹窗打开：一律让位。Esc 由 popup 自己关、输入框要能正常打字、
    // 菜单导航与"弹窗内可用动作"由 ContextMenu 的 Keys.onPressed 处理
    //（它调用 menuAction()/trigger()，与这里是同一张表）。
    if (popups_ && popups_->anyOpen())
        return false;

    auto *ke = static_cast<QKeyEvent *>(event);
    const int idx = keymap::find(ke->key(), ke->modifiers(), mode3d());
    if (idx < 0)
        return false;
    const keymap::Binding &b = keymap::bindings().at(idx);
    if (ke->isAutoRepeat() && !b.allowRepeat)
        return true;   // 吞掉重复：截图/删除/全屏这类不能按住狂触发
    return handle(b, ke->key(), int(ke->modifiers()), false);
}

QString KeyRouter::menuAction(int key, int modifiers) const
{
    const int idx = keymap::findMenu(key, Qt::KeyboardModifiers(modifiers));
    if (idx < 0)
        return QString();
    return keymap::bindings().at(idx).id;
}

bool KeyRouter::trigger(const QString &id)
{
    const int idx = keymap::indexOfAction(id);
    if (idx < 0) {
        MLOG_ERR("key: unknown action '%s'", qUtf8Printable(id));
        return false;
    }
    // 菜单（唯一的外部调用方）里弹窗一定是开着的
    return handle(keymap::bindings().at(idx), 0, 0, true);
}

bool KeyRouter::handle(const keymap::Binding &b, int key, int mods, bool fromMenu)
{
    logDispatch(b.id, key, mods, fromMenu, b.scope);
    switch (b.handler) {
    case keymap::Handler::Swallow:
        MLOG_VERBOSE("key: %s swallowed", qUtf8Printable(b.id));
        return true;
    case keymap::Handler::Qml:
        if (fromMenu) {
            // 菜单只派发 popupOk 的动作，自检保证它们都在 C++ 侧实现
            MLOG_ERR("key: '%s' is a UI action but came from the menu",
                     qUtf8Printable(b.id));
            return false;
        }
        emit actionRequested(b.id);
        return true;
    case keymap::Handler::Cpp:
        break;
    }
    return exec(b.id);
}

bool KeyRouter::exec(const QString &id)
{
    if (!vm_)
        return false;

    // ── 回放 ──────────────────────────────────────────────────────
    if (id == "play-pause") { vm_->togglePlayPause(); return true; }
    if (id == "seek-back") { vm_->seekRelative(-5000); return true; }
    if (id == "seek-back-10") { vm_->seekRelative(-10000); return true; }
    if (id == "seek-fwd") { vm_->seekRelative(5000); return true; }
    if (id == "seek-fwd-10") { vm_->seekRelative(10000); return true; }
    if (id == "volume-up") { vm_->nudgeVolume(0.05); return true; }
    if (id == "volume-down") { vm_->nudgeVolume(-0.05); return true; }
    if (id == "screenshot") { vm_->screenshot(); return true; }
    if (id == "frame-step-fwd") { vm_->frameStep(1); return true; }
    if (id == "frame-step-back") { vm_->frameStep(-1); return true; }
    if (id == "speed-half") { vm_->setSpeed(0.5); return true; }
    if (id == "speed-double") { vm_->setSpeed(2.0); return true; }
    if (id == "speed-normal") { vm_->setSpeed(1.0); return true; }
    if (id == "playlist-prev") { vm_->playlistPrev(); return true; }
    if (id == "playlist-next") { vm_->playlistNext(); return true; }
    if (id == "toggle-osd") { vm_->toggleOsd(); return true; }
    if (id == "toggle-subtitles") { vm_->toggleSubtitles(); return true; }
    if (id == "toggle-fullscreen") { vm_->toggleFullscreen(); return true; }
    if (id == "remove-from-playlist") { vm_->removeCurrentFromPlaylist(); return true; }
    if (id == "delete-file") { vm_->deleteCurrentFile(); return true; }
    if (id == "quit") { QCoreApplication::quit(); return true; }

    // ── 字幕/音频延迟（弹窗内也可用，见 popupOk）───────────────────
    if (id == "sub-delay-down") { vm_->adjustSubDelay(-0.05); return true; }
    if (id == "sub-delay-up") { vm_->adjustSubDelay(0.05); return true; }
    if (id == "sub-delay-reset") { vm_->resetSubDelay(); return true; }
    if (id == "audio-delay-up") { vm_->adjustAudioDelay(0.05); return true; }
    if (id == "audio-delay-down") { vm_->adjustAudioDelay(-0.05); return true; }
    if (id == "audio-delay-reset") { vm_->resetAudioDelay(); return true; }

    // ── 画面比例（照抄 mpv vf/或者 video-aspect-override 的循环）────
    if (id == "aspect-next") { cycleAspect(false); return true; }
    if (id == "aspect-prev") { cycleAspect(true); return true; }

    // ── 3D / 单眼 ─────────────────────────────────────────────────
    if (id == "stereo-layout") {
        if (stereo_) { stereo_->cycleLayout(); syncStereoSource(); }
        return true;
    }
    if (id == "stereo-swap") {
        if (!stereo_)
            return true;
        if (stereo_->active()) {
            stereo_->swapEyes();
        } else {
            // 单眼模式：K = 换到另一只眼
            vm_->setEyeState(vm_->eyeMode() == 2 ? 1 : 2, stereo_->layout(),
                             stereo_->half() ? 1 : 0);
        }
        return true;
    }
    if (id == "stereo-half") {
        if (stereo_) { stereo_->toggleHalf(); syncStereoSource(); }
        return true;
    }
    if (id == "toggle-help") {
        if (stereo_) stereo_->toggleHelp();
        return true;
    }

    MLOG_ERR("key: action '%s' has no implementation", qUtf8Printable(id));
    return false;
}

void KeyRouter::cycleAspect(bool reverse)
{
    /// 画面比例循环：`A` 正序 / `Shift+A` 逆序，列表与顺序照抄 mpv
    /// （third_party/mpv/etc/input.conf:156：
    ///  cycle-values video-aspect-override "16:9" "4:3" "2.35:1" "no"）。
    /// 当前值不在列表里时取第一个（= mpv cmd_cycle_values 的 "Picking default"）。
    static const char *kList[] = {"16:9", "4:3", "2.35:1", "no"};
    const int n = int(sizeof(kList) / sizeof(kList[0]));
    const QString cur = vm_->aspect();
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (cur == QLatin1String(kList[i])) idx = i;
    const int dir = reverse ? -1 : 1;
    const int next = (idx < 0) ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
    vm_->setAspect(QString::fromLatin1(kList[next]));
    // 瞬态提示：值交给 QML 组合文案（3D 下显示"每眼比例"，2D/单眼显示"画面比例"）。
    vm_->flashMessage(QString::fromLatin1(kList[next]));
}

void KeyRouter::syncStereoSource()
{
    if (!vm_ || !stereo_)
        return;
    vm_->setStereoMap(stereo_->active(), stereo_->layout(), stereo_->half() ? 1 : 0);
    vm_->setEyeState(vm_->eyeMode(), stereo_->layout(), stereo_->half() ? 1 : 0);
}

void KeyRouter::noteNav(const QString &id)
{
    logDispatch(id, 0, 0, true, keymap::Scope::Menu);
}

void KeyRouter::logDispatch(const QString &id, int key, int mods, bool fromMenu,
                            keymap::Scope scope)
{
    QString seq = QStringLiteral("-");
    if (key != 0) {
        const Qt::KeyboardModifiers m = Qt::KeyboardModifiers(mods);
        // ⚠ `QKeySequence(key, mods)` 是"两段和弦"构造函数（会打印成
        // "Z, Shift+"）；这里必须走 QKeyCombination。
        seq = QKeySequence(QKeyCombination(m, Qt::Key(key)))
                  .toString(QKeySequence::PortableText);
        // PortableText 把字母一律写成大写（"z" 打印成 "Z"）；日志按物理按键
        // 记，无 Shift 的字母显示小写，免得看日志时以为按的是 Shift+Z。
        if (!(m & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            && seq.size() == 1 && seq.at(0).isLetter())
            seq = seq.toLower();
    }
    ++dispatchCount_;
    const char *scopeName = scope == keymap::Scope::ThreeD ? "3d"
                            : scope == keymap::Scope::Menu ? "menu"
                            : scope == keymap::Scope::TwoD ? "2d"
                                                           : "both";
    const QString line = QStringLiteral("#%1 id=%2 key=%3 popup=%4 mode=%5 scope=%6")
                             .arg(dispatchCount_)
                             .arg(id, seq)
                             .arg(fromMenu || (popups_ && popups_->anyOpen()) ? 1 : 0)
                             .arg(mode3d() ? "3d" : "2d")
                             .arg(QLatin1String(scopeName));
    log_.append(line);
    if (log_.size() > 128)
        log_.removeFirst();
    // 用户/调试日志：按下即记录（VERBOSE，日常不可见）
    MLOG_VERBOSE("key: %s -> %s (popup=%d)", qUtf8Printable(seq),
                 qUtf8Printable(id), fromMenu ? 1 : 0);
}

QString KeyRouter::keyLog() const { return log_.join('\n'); }

void KeyRouter::clearKeyLog() { log_.clear(); }

QVariantList KeyRouter::table() const { return keymap::dump(); }

QString KeyRouter::helpKey(const QString &id) const { return keymap::helpKeyFor(id); }

bool KeyRouter::injectKey(const QString &sequence)
{
    const QKeySequence ks = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (ks.count() != 1 || ks[0].key() == 0) {
        MLOG_ERR("key: cannot parse sequence '%s'", qUtf8Printable(sequence));
        return false;
    }
    // 目标窗口 = 焦点窗口（3D 下是 control 窗口，2D 下是主窗口）；
    // 测试进程可能没被合成器聚焦，退回第一个可见窗口。
    QWindow *win = QGuiApplication::focusWindow();
    if (!win) {
        for (QWindow *w : QGuiApplication::topLevelWindows()) {
            if (w->isVisible()) {
                win = w;
                break;
            }
        }
    }
    if (!win) {
        MLOG_ERR("key: no window to inject into");
        return false;
    }
    QKeyEvent ev(QEvent::KeyPress, ks[0].key(), ks[0].keyboardModifiers());
    // 走真实投递链：QCoreApplication::notify → 应用级事件过滤器（本类）
    // → 窗口 → 焦点项（弹窗的 Keys.onPressed 也在这条链上）
    QCoreApplication::sendEvent(win, &ev);
    return true;
}
