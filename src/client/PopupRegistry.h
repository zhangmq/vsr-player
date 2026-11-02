#pragma once
#include <QList>
#include <QObject>

/// 全局弹窗计数 —— "现在有弹窗开着吗"的唯一来源。
///
/// 为什么不是 QML 侧的 `uiLayer.anyPopupOpen`：那是**每窗口**的。3D 下同时
/// 存在 presentation 主窗口与桌面 control 窗口（两份 main.qml），弹窗只开在其
/// 中一个，另一个窗口的窗口级动作必须同样让位——否则菜单在桌面开着时，按
/// `V`/`S`/`P` 仍会打到视频上（2026-11-02 用户报"上/下 还在调音量"就是
/// 同一类问题，当时只修了单窗口作用域）。
///
/// 用法：Popup 实例创建时调一次 `popups.watch(popup)`（PopupBase 里统一做），
/// 之后由 `opened` 属性的变化信号驱动刷新，不依赖 QML 的 onOpened/onClosed
/// ——那两个 handler 会被实例自己的 `onOpened:` 覆盖掉（main.qml 里每个
/// 弹窗都写了 `onOpened: uiLayer.closeOtherPopups(...)`）。
class PopupRegistry : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool anyOpen READ anyOpen NOTIFY changed)

public:
    explicit PopupRegistry(QObject *parent = nullptr);

    /// 登记一个 Popup（幂等）。之后它的 opened 变化会刷新计数。
    Q_INVOKABLE void watch(QObject *popup);

    int count() const { return openCount_; }
    bool anyOpen() const { return openCount_ > 0; }

public slots:
    /// 重新统计（所有被 watch 的弹窗的 opened 属性）。由 openedChanged 触发。
    void refresh();

signals:
    void changed();

private:
    void recompute(bool notify);

    QList<QObject *> watched_;
    int openCount_ = 0;
};
