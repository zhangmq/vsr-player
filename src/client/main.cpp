/// VSR Player — Qt client entry point.

#include <cstdio>
#include <cstring>

#include <QApplication>

#include "MainWindow.h"
#include "api/Player.h"

int main(int argc, char* argv[]) {
    // Parse flags.  Qt may modify argc/argv during QApplication construction,
    // so we parse first and store results as local values.
    bool use_vsr = true;
    bool no_hwaccel = false;
    vsr::Quality quality = vsr::Quality::HIGH;
    QString file_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-vsr") == 0) {
            use_vsr = false;
        } else if (strcmp(argv[i], "--no-hwaccel") == 0) {
            no_hwaccel = true;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            const char* q = argv[++i];
            if (strcmp(q, "LOW") == 0)      quality = vsr::Quality::LOW;
            else if (strcmp(q, "MEDIUM") == 0) quality = vsr::Quality::MEDIUM;
            else if (strcmp(q, "HIGH") == 0)   quality = vsr::Quality::HIGH;
            else if (strcmp(q, "ULTRA") == 0)  quality = vsr::Quality::ULTRA;
            else fprintf(stderr, "VSR: unknown quality '%s' — using HIGH\n", q);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [--no-vsr] [--no-hwaccel] [--quality LOW|MEDIUM|HIGH|ULTRA] <video-file>\n",
                    argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            file_path = QString::fromLocal8Bit(argv[i]);
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    fprintf(stderr, "VSR: %s quality=%s hwaccel=%s\n",
            use_vsr ? "enabled" : "disabled",
            quality == vsr::Quality::LOW    ? "LOW" :
            quality == vsr::Quality::MEDIUM ? "MEDIUM" :
            quality == vsr::Quality::HIGH   ? "HIGH" : "ULTRA",
            no_hwaccel ? "disabled" : "enabled");

    // Create window and show it.  show() synchronously creates the
    // native Wayland surface on both X11 and Wayland platforms, so
    // the surface is valid by the time show() returns.
    vsr::MainWindow window;
    window.show();

    // Initialize the Core engine with CLI-supplied (or default) settings.
    // This must happen after show() — the native surface is needed for
    // Vulkan init — but BEFORE app.exec() so the player is ready when the
    // event loop starts.
    window.init_player(use_vsr, quality, no_hwaccel);
    if (!file_path.isEmpty())
        window.open_file(file_path);

    return app.exec();
}