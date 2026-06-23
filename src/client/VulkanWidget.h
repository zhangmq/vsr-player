#pragma once

#include <QWidget>

namespace vsr {

/// Minimal QWidget that provides a Wayland surface (wl_surface) for
/// the Core to render into via Vulkan. No Vulkan logic lives here —
/// all rendering is handled by PlayerCore on its worker thread.
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);

    QPaintEngine* paintEngine() const override { return nullptr; }
};

}  // namespace vsr
