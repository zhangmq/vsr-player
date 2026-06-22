#include "VulkanContext.h"

namespace vsr {

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() { release(); }

bool VulkanContext::init(void*) { return false; }
void VulkanContext::release() {}

}  // namespace vsr
