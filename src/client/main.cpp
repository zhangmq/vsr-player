#include <QApplication>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("VSR Player");
    app.setApplicationVersion("0.1.0");

    // Parse args — first positional arg is video file
    QStringList args = app.arguments();
    QString video_file;
    if (args.size() > 1) {
        video_file = args.at(1);
    }

    vsr::MainWindow window;
    window.show();

    if (!video_file.isEmpty()) {
        window.open_file(video_file);
    }

    return app.exec();
}
