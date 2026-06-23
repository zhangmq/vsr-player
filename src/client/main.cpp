/// VSR Player — Qt client entry point.

#include <cstring>

#include <QApplication>

#include "MainWindow.h"
#include "api/Player.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    // Parse flags
    bool use_vsr = true;
    bool no_hwaccel = false;
    vsr::Quality quality = vsr::Quality::HIGH;
    int file_arg = 1;
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
        } else {
            file_arg = i;
        }
    }

    fprintf(stderr, "VSR: %s quality=%s hwaccel=%s\n",
            use_vsr ? "enabled" : "disabled",
            quality == vsr::Quality::LOW ? "LOW" :
            quality == vsr::Quality::MEDIUM ? "MEDIUM" :
            quality == vsr::Quality::HIGH ? "HIGH" : "ULTRA",
            no_hwaccel ? "disabled" : "enabled");

    vsr::MainWindow window(use_vsr, quality);
    if (no_hwaccel) window.set_no_hwaccel(true);
    window.show();

    if (file_arg < argc) {
        window.open_file(argv[file_arg]);
    }

    return app.exec();
}
