#include "VulkanWidget.h"

#include <QShowEvent>
#include <cstdio>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VulkanWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Native window handle is valid at this point.
    // Emit signal to inform MainWindow that the client is ready for player init.
    if (winId() != 0) {
        fprintf(stderr, "VulkanWidget: native window ready (winId=%p)\n",
                reinterpret_cast<void*>(winId()));
        emit nativeWindowReady();
    } else {
        fprintf(stderr, "VulkanWidget: showEvent but winId is 0 — "
                "platform may not support native windows\n");
    }
}

}  // namespace vsr
