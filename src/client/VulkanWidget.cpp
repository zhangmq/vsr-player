#include "VulkanWidget.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QResizeEvent>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VulkanWidget::~VulkanWidget() = default;

bool VulkanWidget::init_vulkan() {
    // winId() returns the X11 Window ID.
    // For X11: pass the Window + X11 Display (from QX11NativeInterface).
    // For Wayland: pass wl_surface + wl_display.
    WId wid = winId();

    return renderer_.init(reinterpret_cast<void*>(wid), nullptr);
}

bool VulkanWidget::present_frame(const uint8_t* rgb_data, int width, int height) {
    if (!renderer_.is_ready()) {
        if (!init_vulkan()) return false;
        renderer_.resize(width, height);
    }
    if (width != frame_width_ || height != frame_height_) {
        renderer_.resize(width, height);
        frame_width_ = width;
        frame_height_ = height;
    }
    return renderer_.render_frame(rgb_data, width, height);
}

void VulkanWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

void VulkanWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!vulkan_ready_) {
        vulkan_ready_ = true;
    }
}

}  // namespace vsr
