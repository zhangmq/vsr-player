#include "VulkanWidget.h"

#include <wayland-client.h>

#include <QGuiApplication>
#include <cstdio>
#include <cstdlib>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VulkanWidget::~VulkanWidget() = default;

bool VulkanWidget::init_vulkan() {
    QString qpa = QGuiApplication::platformName();
    const char* session = getenv("XDG_SESSION_TYPE");

    if (qpa == "wayland" || (session && strcmp(session, "wayland") == 0)) {
        // Wayland: Qt creates the wl_surface for us via WA_NativeWindow.
        // Connect to the same Wayland display via the standard env var.
        // The compositor multiplexes connections — separate wl_display
        // connections to the same socket share the same server context.
        void* display = wl_display_connect(nullptr);
        void* window  = reinterpret_cast<void*>(winId());
        fprintf(stderr, "VulkanWidget: wayland display=%p surface=%p\n",
                display, window);
        return renderer_.init(Platform::WAYLAND, window, display);
    } else {
        void* window = reinterpret_cast<void*>(winId());
        return renderer_.init(Platform::XLIB, window, nullptr);
    }
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
