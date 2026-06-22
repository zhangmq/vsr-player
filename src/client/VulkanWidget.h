#pragma once

#include <QWidget>

#include "utils/VulkanRenderer.h"

namespace vsr {

/// QWidget embedding a Vulkan render surface.
///
/// Disables Qt painting, provides winId() as the VkSurfaceKHR target.
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);
    ~VulkanWidget() override;

    /// Initialize Vulkan and create surface from this widget's native window.
    bool init_vulkan();

    /// Render one RGB24 frame.
    bool present_frame(const uint8_t* rgb_data, int width, int height);

    QPaintEngine* paintEngine() const override { return nullptr; }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    VulkanRenderer renderer_;
    bool vulkan_ready_ = false;
    int frame_width_ = 0;
    int frame_height_ = 0;
};

}  // namespace vsr
