/// VSR Player — Qt client entry point.

#include <QApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // NVIDIA Vulkan driver: VK_KHR_wayland_surface present support = false.
    // We must use X11 surfaces, so Qt needs to run on XCB (XWayland).
    // This is transparent — the window still appears as a native Wayland
    // window through the compositor's XWayland bridge.
    const char* session = getenv("XDG_SESSION_TYPE");
    if (session && strcmp(session, "wayland") == 0) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            fprintf(stderr, "Note: NVIDIA Vulkan → forcing XCB QPA (XWayland)\n");
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    }

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
