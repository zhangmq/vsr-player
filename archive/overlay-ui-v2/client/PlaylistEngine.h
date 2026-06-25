#pragma once

#include <QObject>
#include <QStringList>

namespace vsr {

class PlaylistEngine : public QObject {
    Q_OBJECT

public:
    explicit PlaylistEngine(QObject* parent = nullptr);

    int scanFolder(const QString& rootPath, int maxDepth = 3);

    const QStringList& files() const { return files_; }
    int currentIndex() const { return currentIndex_; }
    void setCurrentIndex(int idx);

    QString next();
    QString previous();

    int indexOf(const QString& path) const;
    int count() const { return files_.size(); }
    QString currentFile() const;

signals:
    void scanFinished(int fileCount);

private:
    void scanDirRecursive(const QString& dir, int depth, int maxDepth);

    QStringList files_;
    int currentIndex_ = -1;

    static const QStringList kVideoExtensions;
};

}  // namespace vsr
