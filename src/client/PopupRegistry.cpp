#include "PopupRegistry.h"

#include <QMetaProperty>
#include <QVariant>

PopupRegistry::PopupRegistry(QObject *parent) : QObject(parent) {}

void PopupRegistry::watch(QObject *popup)
{
    if (!popup || watched_.contains(popup))
        return;
    const int idx = popup->metaObject()->indexOfProperty("opened");
    if (idx < 0) {
        // 不是 Popup（例如 Drawer 的内部项）；不参与计数
        return;
    }
    watched_.append(popup);
    // 字符串连接：opened 的通知信号是 moc 生成的 openedChanged()，
    // 用 SIGNAL()/SLOT() 才不需要知道具体类型（QQuickPopup 在 QtQuick 模块里，
    // 客户端不想为这一处链接 QtQuick 私有头）。
    connect(popup, SIGNAL(openedChanged()), this, SLOT(refresh()));
    connect(popup, &QObject::destroyed, this, [this, popup]() {
        watched_.removeAll(popup);
        recompute(true);
    });
    recompute(true);
}

void PopupRegistry::refresh() { recompute(true); }

void PopupRegistry::recompute(bool notify)
{
    int n = 0;
    for (QObject *p : watched_) {
        const int idx = p->metaObject()->indexOfProperty("opened");
        if (idx < 0)
            continue;
        if (p->metaObject()->property(idx).read(p).toBool())
            ++n;
    }
    if (n == openCount_)
        return;
    openCount_ = n;
    if (notify)
        emit changed();
}
