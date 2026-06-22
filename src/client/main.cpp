/// VSR Player — Qt client entry point.

#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // We probe Vulkan Wayland surface support at runtime.
    // If VK_KHR_wayland_surface is not supported by the physical device,
    // VulkanWidget will detect the failure and inform the user.
    // Native Wayland works when nvidia_drm.modeset=1 is set (kernel cmdline).
    // With modeset=0, VkSurfaceSupportKHR returns false for Wayland surfaces
    // even though the extension is present at instance level.

    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    vsr::MainWindow window;
    window.show();

    if (argc > 1) {
        window.open_file(argv[1]);
    }

    return app.exec();
}
