/// VSR Player — Qt client entry point.

#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QFileInfo>

#include "MainWindow.h"
#include "api/Player.h"

int main(int argc, char* argv[]) {
    bool use_vsr = true;
    bool no_hwaccel = false;
    vsr::Quality quality = vsr::Quality::HIGH;
    QString file_path;
    int depth = 3;

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
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            depth = atoi(argv[++i]);
            if (depth < 1) depth = 1;
            if (depth > 10) depth = 10;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [options] <video-file|folder>\n"
                    "Options:\n"
                    "  --no-vsr                Disable AI super-resolution\n"
                    "  --no-hwaccel            Disable NVDEC hardware decoding\n"
                    "  --quality LOW|MEDIUM|HIGH|ULTRA  VSR quality (default HIGH)\n"
                    "  --depth N               Folder scan depth (default 3, max 10)\n",
                    argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            file_path = QString::fromLocal8Bit(argv[i]);
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    fprintf(stderr, "VSR: %s quality=%s hwaccel=%s depth=%d\n",
            use_vsr ? "enabled" : "disabled",
            quality == vsr::Quality::LOW    ? "LOW" :
            quality == vsr::Quality::MEDIUM ? "MEDIUM" :
            quality == vsr::Quality::HIGH   ? "HIGH" : "ULTRA",
            no_hwaccel ? "disabled" : "enabled",
            depth);

    vsr::MainWindow window;
    window.show();

    window.init_player(use_vsr, quality, no_hwaccel);

    if (!file_path.isEmpty()) {
        QFileInfo fi(file_path);
        if (fi.isDir()) {
            window.open_folder(file_path, depth);
        } else {
            window.open_file(file_path);
        }
    }

    return app.exec();
}
