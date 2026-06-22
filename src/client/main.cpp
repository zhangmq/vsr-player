/// VSR Player — Qt client entry point.
///
/// Build:
///   g++ -std=c++20 -O2 -Wall -o build/vsr-player \
///       src/client/main.cpp src/client/MainWindow.cpp \
///       src/client/VulkanWidget.cpp \
///       src/core/Demuxer.cpp src/core/Decoder.cpp \
///       src/core/utils/VulkanRenderer.cpp \
///       $(pkg-config --cflags --libs Qt6Widgets vulkan \
///         libavcodec libavformat libavutil libswscale) \
///       -lcuda -Isrc/core -Isrc/core/api -Isrc/client -Isrc/core/utils

#include <QApplication>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    vsr::MainWindow window;
    window.show();

    // Open file from command line
    if (argc > 1) {
        window.open_file(argv[1]);
    }

    return app.exec();
}
