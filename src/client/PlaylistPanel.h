#pragma once

#include <QWidget>

class QListView;
class QStringListModel;

namespace vsr {

/// Playlist panel with file list. Drag-drop files or open via dialog.
class PlaylistPanel : public QWidget {
    Q_OBJECT

public:
    explicit PlaylistPanel(QWidget* parent = nullptr);

    void add_file(const QString& path);
    void add_files(const QStringList& paths);
    QString current_file() const;
    void clear();

signals:
    void file_activated(const QString& path);

private:
    QListView* view_ = nullptr;
    QStringListModel* model_ = nullptr;
};

}  // namespace vsr
