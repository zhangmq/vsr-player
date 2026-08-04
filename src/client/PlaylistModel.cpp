#include "PlaylistModel.h"

#include <QFileInfo>

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : paths_.size();
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= paths_.size())
        return {};
    switch (role) {
    case Qt::DisplayRole: return display_[index.row()];
    case PathRole:        return paths_[index.row()];
    }
    return {};
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    // 内建 DisplayRole → "display"（QML 里 model.display = 显示名）
    QHash<int, QByteArray> names = QAbstractListModel::roleNames();
    names.insert(PathRole, "path");
    return names;
}

QString PlaylistModel::displayName(const QString &path)
{
    // 本地文件：basename。URL/异常输入：取最后一段，空则原样返回。
    QString name = QFileInfo(path).fileName();
    if (name.isEmpty()) {
        const int slash = path.lastIndexOf('/');
        if (slash >= 0 && slash + 1 < path.size())
            name = path.mid(slash + 1);
        else
            name = path;
    }
    return name;
}

void PlaylistModel::setSnapshot(const QStringList &paths, int current)
{
    // ── 前缀 diff：公共前缀保留，尾部增删增量通知 ──
    const int oldN = paths_.size();
    const int newN = paths.size();
    int keep = 0;
    while (keep < oldN && keep < newN && paths[keep] == paths_[keep])
        keep++;

    // 注意：begin/end 不得嵌套（Qt 断言/信号错乱）——remove 与 insert
    // 各自完整成对（begin → 改数据 → end），互不交叉。
    if (oldN > keep) {
        beginRemoveRows({}, keep, oldN - 1);
        paths_.resize(keep);
        display_.resize(keep);
        endRemoveRows();
    }
    if (newN > keep) {
        beginInsertRows({}, keep, newN - 1);
        for (int i = keep; i < newN; i++) {
            paths_.append(paths[i]);
            display_.append(displayName(paths[i]));
        }
        endInsertRows();
    }
    if (oldN != newN) emit countChanged();

    // current 变化：只刷新旧/新行高亮，不重建列表
    if (current != current_) {
        if (current_ >= 0 && current_ < paths_.size())
            emit dataChanged(index(current_), index(current_));
        current_ = current;
        if (current_ >= 0 && current_ < paths_.size())
            emit dataChanged(index(current_), index(current_));
        emit currentIndexChanged();
    }
}
