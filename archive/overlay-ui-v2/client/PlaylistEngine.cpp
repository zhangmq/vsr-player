#include "PlaylistEngine.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cstdio>

namespace vsr {

const QStringList PlaylistEngine::kVideoExtensions = {
    "mp4", "mkv", "webm", "avi", "mov", "ts", "m2ts", "flv", "wmv", "mpg", "mpeg"
};

PlaylistEngine::PlaylistEngine(QObject* parent) : QObject(parent) {}

int PlaylistEngine::scanFolder(const QString& rootPath, int maxDepth) {
    files_.clear();
    currentIndex_ = -1;

    QFileInfo fi(rootPath);
    if (!fi.isDir()) return 0;

    scanDirRecursive(rootPath, 0, maxDepth);

    std::sort(files_.begin(), files_.end(),
              [](const QString& a, const QString& b) {
                  return QFileInfo(a).fileName().compare(
                      QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0;
              });

    emit scanFinished(files_.size());
    return files_.size();
}

void PlaylistEngine::scanDirRecursive(const QString& dir, int depth,
                                       int maxDepth) {
    if (depth > maxDepth) return;

    QDir d(dir);
    if (!d.isReadable()) {
        fprintf(stderr, "PlaylistEngine: cannot read dir '%s', skipping\n",
                qPrintable(dir));
        return;
    }

    auto entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::Name | QDir::DirsFirst);
    for (const auto& fi : entries) {
        if (fi.isDir()) {
            scanDirRecursive(fi.absoluteFilePath(), depth + 1, maxDepth);
        } else if (fi.isFile()) {
            QString ext = fi.suffix().toLower();
            if (kVideoExtensions.contains(ext)) {
                files_.append(fi.absoluteFilePath());
            }
        }
    }
}

void PlaylistEngine::setCurrentIndex(int idx) {
    if (idx >= 0 && idx < files_.size())
        currentIndex_ = idx;
}

QString PlaylistEngine::next() {
    if (files_.isEmpty()) return {};
    if (currentIndex_ < 0) {
        currentIndex_ = 0;
        return files_.at(0);
    }
    if (currentIndex_ + 1 >= files_.size()) return {};
    currentIndex_++;
    return files_.at(currentIndex_);
}

QString PlaylistEngine::previous() {
    if (files_.isEmpty() || currentIndex_ <= 0) return {};
    currentIndex_--;
    return files_.at(currentIndex_);
}

int PlaylistEngine::indexOf(const QString& path) const {
    return files_.indexOf(path);
}

QString PlaylistEngine::currentFile() const {
    if (currentIndex_ < 0 || currentIndex_ >= files_.size()) return {};
    return files_.at(currentIndex_);
}

}  // namespace vsr
