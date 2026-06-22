#include "VulkanWidget.h"

#include <QResizeEvent>

namespace vsr {

VulkanWidget::VulkanWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    // TODO: create Vulkan surface via QVulkanInstance or manual
}

VulkanWidget::~VulkanWidget() {
    // TODO: destroy Vulkan surface
}

void VulkanWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // TODO: notify player core of resize
}

bool VulkanWidget::init_vulkan() {
    // TODO: create VkSurfaceKHR from QWidget::winId()
    return false;
}

}  // namespace vsr
