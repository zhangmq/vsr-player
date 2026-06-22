#include "Renderer.h"

namespace vsr {

Renderer::Renderer() = default;
Renderer::~Renderer() { release(); }

bool Renderer::init(void*, int width, int height) {
    width_ = width;
    height_ = height;
    // TODO: create Vulkan instance, device, swapchain, render pass
    return false;
}

bool Renderer::render_frame(void*, int, int) {
    // TODO: CUDA→Vulkan interop, submit command buffer, present
    return false;
}

bool Renderer::resize(int width, int height) {
    width_ = width;
    height_ = height;
    // TODO: recreate swapchain
    return false;
}

void Renderer::release() {
    // TODO: destroy Vulkan resources
}

}  // namespace vsr
