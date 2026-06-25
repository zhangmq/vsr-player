#pragma once

#include <QObject>
#include <QStringList>

/// Playlist engine — folder traversal + playlist navigation.
/// No Qt UI dependency beyond QObject for QML property binding.
class PlaylistEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QStringList displayNames READ displayNames NOTIFY filesChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(int count READ count NOTIFY filesChanged)
public:
    explicit PlaylistEngine(QObject* parent = nullptr);

    /// Scan a folder for video files. Returns number of files found.
    Q_INVOKABLE int scanFolder(const QString& path, int depth = 3);

    /// Navigate: returns the new current file path or empty if at boundary.
    Q_INVOKABLE QString next();
    Q_INVOKABLE QString previous();

    /// Set current index to the given file path. Used by playlist panel
    /// to highlight the clicked item. Does NOT trigger LOAD_FILE.
    Q_INVOKABLE void setCurrentFile(const QString& path);

    /// Accessors for QML.
    QStringList files() const { return files_; }
    QStringList displayNames() const { return displayNames_; }
    int currentIndex() const { return currentIndex_; }
    QString currentFile() const;
    int count() const { return files_.size(); }

signals:
    void filesChanged();
    void currentIndexChanged();
    void currentFileChanged();

private:
    void scanDir(const QString& path, int remainingDepth);

    QStringList files_;
    QStringList displayNames_;
    int currentIndex_ = -1;
    QString rootPath_;
};
