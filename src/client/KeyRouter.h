#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "Keymap.h"

class PlayerViewModel;
class StereoController;
class PopupRegistry;

/// 按键派发中心 —— 键位表（Keymap.cpp）的唯一执行者。
///
/// 机制：`qApp->installEventFilter` 应用级过滤器。它比 QShortcutMap 更早看到
/// KeyPress（3D 的 Esc 一直依赖这一点），因此可以直接吞键，也不需要 QShortcut /
/// QML Shortcut（会随窗口数量重复注册、还会打 "Ambiguous shortcut" 告警）。
///
/// 弹窗打开时**整体让位**（`popups->anyOpen()`）：Esc 关弹窗、输入框正常打字、
/// 菜单导航交给 ContextMenu 自己的 Keys.onPressed。全局计数见 PopupRegistry。
///
/// 动作分两类：C++ 能直接做的（viewModel/stereo 调用）在这里执行；只有 QML 里
/// 才存在的东西（对话框、播放列表面板、右键菜单）通过 actionRequested 发给 UI 层，
/// 由两个窗口里"当前该干活的"那个执行（见 main.qml 的 handlesActions）。
class KeyRouter : public QObject
{
    Q_OBJECT

public:
    KeyRouter(PlayerViewModel *vm, StereoController *stereo, PopupRegistry *popups,
              QObject *parent = nullptr);

    bool eventFilter(QObject *watched, QEvent *event) override;

    /// 菜单里的按键 → 动作 id（""=不处理）。菜单导航键与"弹窗内可用的动作"
    /// （字幕/音频延迟）都从这里查表，不再另写一份映射。
    Q_INVOKABLE QString menuAction(int key, int modifiers) const;

    /// 执行动作（C++ 直接执行；Qml 动作转成 actionRequested 信号）
    Q_INVOKABLE bool trigger(const QString &id);

    /// 测试钩子：把一条按键序列（QKeySequence 语法，如 "Shift+Z"）注入当前窗口。
    /// 走真实投递链（应用级过滤器 → 焦点项），所以能覆盖"弹窗打开时按键去哪了"
    /// 这类只在真按键下才暴露的问题。
    Q_INVOKABLE bool injectKey(const QString &sequence);

    /// 菜单导航（menu-*）由 ContextMenu 自己执行，这里只记账——RPC key-log
    /// 靠它才能断言"打开菜单后按 ↓ 走的是菜单导航而不是调音量"。
    Q_INVOKABLE void noteNav(const QString &id);

    /// 帮助面板/文档：动作 id 的键名（表里声明，缺省取第一个序列）
    Q_INVOKABLE QString helpKey(const QString &id) const;

    /// 键位表快照（RPC get-keys / 静态检查 / 文档生成）
    Q_INVOKABLE QVariantList table() const;

    /// 最近派发记录（RPC get-key-log）：`#n id popup=0/1 mode=2d/3d`
    Q_INVOKABLE QString keyLog() const;
    Q_INVOKABLE void clearKeyLog();

signals:
    /// QML 侧动作（对话框/面板/菜单）
    void actionRequested(const QString &id);

private:
    bool mode3d() const;
    bool handle(const keymap::Binding &b, int key, int mods, bool fromMenu);
    bool exec(const QString &id);
    void cycleAspect(bool reverse);
    void syncStereoSource();
    void logDispatch(const QString &id, int key, int mods, bool fromMenu,
                     keymap::Scope scope = keymap::Scope::TwoD);

    PlayerViewModel *vm_ = nullptr;
    StereoController *stereo_ = nullptr;
    PopupRegistry *popups_ = nullptr;
    QStringList log_;
    int dispatchCount_ = 0;
};
