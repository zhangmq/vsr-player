#pragma once

#include <QWidget>

namespace vsr {

/// Minimal QWidget that provides a Wayland surface (wl_surface) for
/// the Core to render into via Vulkan. No Vulkan logic lives here —
/// all rendering is handled by PlayerCore on its worker thread.
///
/// Emits nativeWindowReady() from showEvent() when the native
/// Wayland surface has been created and winId() is valid.
/// This is the "client ready" signal that gates player initialization.
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);

    QPaintEngine* paintEngine() const override { return nullptr; }

    /// Whether the native window handle is valid.
    bool isNativeReady() const { return winId() != 0; }

signals:
    /// Emitted when the native Wayland surface is created.
    void nativeWindowReady();

protected:
    void showEvent(QShowEvent* event) override;
};

}  // namespace vsr
