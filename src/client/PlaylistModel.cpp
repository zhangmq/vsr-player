#include "PlaylistModel.h"
#include "BlurayDisc.h"

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
    case ErrorCodeRole:   return errors_.value(index.row(), 0);
    case IsFolderRole:    return folders_.value(index.row(), false);
    }
    return {};
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    // 内建 DisplayRole → "display"（QML 里 model.display = 显示名）
    QHash<int, QByteArray> names = QAbstractListModel::roleNames();
    names.insert(PathRole, "path");
    names.insert(ErrorCodeRole, "errorCode");
    names.insert(IsFolderRole, "isFolder");
    return names;
}

void PlaylistModel::setErrors(const QVector<int> &errors)
{
    const int n = paths_.size();
    if (errors.size() != n || n == 0) return;
    errors_.resize(n);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (errors_[i] != errors[i]) { errors_[i] = errors[i]; changed = true; }
    }
    if (changed)
        emit dataChanged(index(0), index(n - 1), {ErrorCodeRole});
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

void PlaylistModel::setSnapshot(const QStringList &paths, int current,
                                const QStringList &titles,
                                const QVector<int> &errors)
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
        titles_.resize(keep);
        if (errors_.size() > keep) errors_.resize(keep);
        if (folders_.size() > keep) folders_.resize(keep);
        endRemoveRows();
    }
    if (newN > keep) {
        beginInsertRows({}, keep, newN - 1);
        for (int i = keep; i < newN; i++) {
            paths_.append(paths[i]);
            const QString t = i < titles.size() ? titles[i] : QString();
            titles_.append(t);
            display_.append(t.isEmpty() ? displayName(paths[i]) : t);
            // 目录判定只对本地路径有意义（URL/流为 false）——用于文案区分
            // "空文件夹" 与 "无法播放"，不参与播放决策。
            folders_.append(QFileInfo(paths[i]).isDir());
        }
        endInsertRows();
    }
    if (oldN != newN) emit countChanged();

    // 失败标记：与行对齐写入；只在值真的变化时发 dataChanged（切歌不重绘）。
    if (errors.size() == newN) {
        if (errors_.size() != newN) errors_.resize(newN);
        bool changedAny = false;
        for (int i = 0; i < newN; i++) {
            if (errors_[i] != errors[i]) { errors_[i] = errors[i]; changedAny = true; }
        }
        if (changedAny && newN > 0)
            emit dataChanged(index(0), index(newN - 1), {ErrorCodeRole});
    }

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
