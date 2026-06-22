#pragma once

#include <QWidget>

#include "utils/VulkanRenderer.h"

namespace vsr {

/// QWidget embedding a Vulkan render surface (Wayland or X11).
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);
    ~VulkanWidget() override;

    bool init_vulkan();
    bool present_frame(const uint8_t* rgb_data, int width, int height);

    QPaintEngine* paintEngine() const override { return nullptr; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    VulkanRenderer renderer_;
    bool vulkan_ready_ = false;
};

}  // namespace vsr
