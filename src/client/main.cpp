/// VSR Player — Qt client entry point.

#include <cstring>

#include <QApplication>
#include <QTimer>

#include "MainWindow.h"
#include "api/Player.h"

int main(int argc, char* argv[]) {
    // Parse flags BEFORE QApplication — Qt may modify argc/argv.
    bool use_vsr = true;
    vsr::Quality quality = vsr::Quality::HIGH;
    int file_arg = 0;  // 0 = no file
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-vsr") == 0) {
            use_vsr = false;
        } else if (strcmp(argv[i], "--no-hwaccel") == 0) {
            fprintf(stderr, "VSR: --no-hwaccel not yet wired in core API\n");
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
            file_arg = i;
        }
    }

    // Now Qt can safely consume its own args
    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    fprintf(stderr, "VSR: %s quality=%s\n",
            use_vsr ? "enabled" : "disabled",
            quality == vsr::Quality::LOW ? "LOW" :
            quality == vsr::Quality::MEDIUM ? "MEDIUM" :
            quality == vsr::Quality::HIGH ? "HIGH" : "ULTRA");

    // Create window
    vsr::MainWindow window;
    window.show();

    // Defer player init + file load to the first event loop iteration,
    // after the native Wayland surface has been created.
    QTimer::singleShot(0, [&window, use_vsr, quality] {
        window.init_player(use_vsr, quality);
    });
    if (file_arg > 0) {
        QTimer::singleShot(0, [&window, path = QString::fromLocal8Bit(argv[file_arg])] {
            window.open_file(path);
        });
    }

    return app.exec();
}
