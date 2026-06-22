#include "VulkanWidget.h"

#include <QGuiApplication>
#include <cstdio>
#include <cstdlib>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VulkanWidget::~VulkanWidget() = default;

bool VulkanWidget::init_vulkan() {
    // Detect platform: check Qt platform name + env vars
    QString qpa = QGuiApplication::platformName();
    const char* session = getenv("XDG_SESSION_TYPE");

    Platform plat;
    void* display = nullptr;

    if (qpa == "wayland" || (session && strcmp(session, "wayland") == 0)) {
        plat = Platform::WAYLAND;
        // display = nullptr → let VulkanRenderer connect internally
    } else {
        plat = Platform::XLIB;
    }

    WId wid = winId();
    fprintf(stderr, "VulkanWidget: platform=%s\n",
            plat == Platform::WAYLAND ? "wayland" : "x11");
    return renderer_.init(plat, reinterpret_cast<void*>(wid), display);
}

bool VulkanWidget::present_frame(const uint8_t* rgb_data, int width, int height) {
    if (!renderer_.is_ready()) {
        if (!init_vulkan()) return false;
        renderer_.resize(width, height);
    }
    return renderer_.render_frame(rgb_data, width, height);
}

void VulkanWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    vulkan_ready_ = true;
}

}  // namespace vsr
