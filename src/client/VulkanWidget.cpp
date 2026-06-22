#include "VulkanWidget.h"

#include <QtGui/qpa/qplatformnativeinterface.h>

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
    // Use Qt's wl_display — MUST match the connection that owns
    // the wl_surface from winId(). Wayland objects are per-connection.
    auto* native = QGuiApplication::platformNativeInterface();
    void* display = native->nativeResourceForIntegration("wl_display");
    void* window  = reinterpret_cast<void*>(winId());

    fprintf(stderr, "VulkanWidget: wl_display=%p surface=%p\n",
            display, window);

    if (!display || !window) {
        fprintf(stderr, "VulkanWidget: null display or window handle\n");
        return false;
    }

    if (renderer_.init(window, display)) {
        fprintf(stderr, "VulkanWidget: Vulkan initialized\n");
        return true;
    }

    fprintf(stderr,
            "VulkanWidget: Vulkan init failed.\n"
            "  NVIDIA GPUs require nvidia_drm.modeset=1 for native Wayland.\n"
            "  Verify: cat /sys/module/nvidia_drm/parameters/modeset\n");
    return false;
}

bool VulkanWidget::present_frame(const uint8_t* rgb_data, int video_w, int video_h) {
    if (!vulkan_ready_) return false;
    if (!renderer_.is_ready()) {
        if (!init_vulkan()) return false;
    }
    // Keep swapchain sized to the widget (window), not the video.
    // render_frame will letterbox the video within the swapchain.
    int ww = width(), wh = height();
    if (ww > 0 && wh > 0)
        renderer_.resize(ww, wh);
    return renderer_.render_frame(rgb_data, video_w, video_h);
}

void VulkanWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Initialize Vulkan as soon as the widget has a valid native window.
    if (!vulkan_ready_) {
        vulkan_ready_ = init_vulkan();
    }
}

}  // namespace vsr
