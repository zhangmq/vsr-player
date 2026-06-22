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
        // Native Wayland Vulkan surface.
        // Requires nvidia_drm.modeset=1 for NVIDIA GPUs.
        void* display = wl_display_connect(nullptr);
        void* window  = reinterpret_cast<void*>(winId());
        if (renderer_.init(Platform::WAYLAND, window, display))
            return true;

        // Wayland surface failed. This is expected on NVIDIA when
        // nvidia_drm.modeset=1 is NOT set in kernel cmdline.
        fprintf(stderr,
                "VulkanWidget: Wayland Vulkan surface failed.\n"
                "  NVIDIA GPUs require nvidia_drm.modeset=1 for native Wayland.\n"
                "  Workaround: QT_QPA_PLATFORM=xcb ./build/vsr-player <video>\n");
        return false;
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
