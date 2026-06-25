#include "PlaylistEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

// Common video extensions for filtering.
static const QSet<QString> kExtensions = {
    "mp4", "mkv", "webm", "avi", "mov", "ts", "m2ts", "flv", "wmv",
    "mpg", "mpeg", "m4v", "ogv", "divx", "webp"
};

PlaylistEngine::PlaylistEngine(QObject* parent) : QObject(parent) {}

int PlaylistEngine::scanFolder(const QString& path, int depth) {
    files_.clear();
    currentIndex_ = -1;
    rootPath_ = path;

    QFileInfo fi(path);
    if (fi.isDir()) {
        scanDir(path, depth);
        std::sort(files_.begin(), files_.end());
    } else if (fi.isFile()) {
        files_.append(fi.absoluteFilePath());
    }
    if (!files_.isEmpty()) currentIndex_ = 0;

    emit filesChanged();
    if (!files_.isEmpty()) {
        emit currentIndexChanged();
        emit currentFileChanged();
    }
    return files_.size();
}

void PlaylistEngine::scanDir(const QString& path, int remainingDepth) {
    if (remainingDepth <= 0) return;

    QDir dir(path);
    auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                     QDir::Name | QDir::DirsFirst);

    for (const auto& info : entries) {
        if (info.isDir()) {
            scanDir(info.absoluteFilePath(), remainingDepth - 1);
        } else if (info.isFile()) {
            QString ext = info.suffix().toLower();
            if (kExtensions.contains(ext)) {
                files_.append(info.absoluteFilePath());
            }
        }
    }
}

QString PlaylistEngine::currentFile() const {
    if (currentIndex_ < 0 || currentIndex_ >= files_.size())
        return {};
    return files_[currentIndex_];
}

QString PlaylistEngine::next() {
    if (files_.isEmpty()) return {};
    currentIndex_ = (currentIndex_ + 1) % files_.size();
    emit currentIndexChanged();
    emit currentFileChanged();
    return files_[currentIndex_];
}

QString PlaylistEngine::previous() {
    if (files_.isEmpty()) return {};
    currentIndex_ = (currentIndex_ - 1 + files_.size()) % files_.size();
    emit currentIndexChanged();
    emit currentFileChanged();
    return files_[currentIndex_];
}

void PlaylistEngine::setCurrentFile(const QString& path) {
    int idx = files_.indexOf(path);
    if (idx >= 0 && idx != currentIndex_) {
        currentIndex_ = idx;
        emit currentIndexChanged();
        emit currentFileChanged();
    }
}

#include "moc_PlaylistEngine.cpp"
