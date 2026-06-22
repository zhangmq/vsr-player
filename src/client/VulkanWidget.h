#pragma once

#include <QWidget>

namespace vsr {

/// Embedded Vulkan render surface widget.
/// Owns a Vulkan surface that libvsrplayer renders into.
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);
    ~VulkanWidget() override;

    /// Get the native Vulkan surface handle for the player core.
    void* vulkan_surface() const { return surface_; }

protected:
    QPaintEngine* paintEngine() const override { return nullptr; }
    void resizeEvent(QResizeEvent* event) override;

private:
    bool init_vulkan();
    void* surface_ = nullptr;  // VkSurfaceKHR
};

}  // namespace vsr
