#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QString>

/// 播放列表 model（数据源：mpv `playlist` 属性全量快照，经
/// PlayerViewModel 喂入）。
///
/// 增量更新：setSnapshot 与旧快照做**前缀 diff**——公共前缀保留，
/// 尾部增删用 beginInsertRows/beginRemoveRows（mpv 播放列表典型变化
/// 形态：尾部加载、任意位置移除）；current 变化只 dataChanged 刷新
/// 对应行高亮，不重建列表。旧实现（QStringList Q_PROPERTY）每次切歌
/// 都全量 reset，大列表切歌/滚动卡顿。
///
/// 显示名与完整路径分离：Qt::DisplayRole 返回 basename（列表项显示），
/// PathRole 返回完整路径（tooltip）——同名文件不做消歧（顺序播放列表，
/// 差异由 tooltip 提供）。
class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
    // 必须 Q_PROPERTY 才能被 QML 访问——普通 C++ 方法在 QML 里求值
    // 为 undefined（曾漏声明，导致高亮绑定永远 false、header 计数为空）。
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role {
        PathRole = Qt::UserRole + 1,   // 完整路径（tooltip）
    };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 全量快照替换（增量生效）。paths 为完整路径列表，current 为
    /// 当前播放条目索引（-1 = 无）。
    void setSnapshot(const QStringList &paths, int current);

    int currentIndex() const { return current_; }
    int count() const { return paths_.size(); }
    /// 完整路径 → 列表索引（-1 = 不在列表）。供重播定位：
    /// stop 后列表保留（keep-playlist），重播用 playlist-play-index
    /// 播放原条目而非 loadfile replace（后者清空整个列表）。
    int indexOfPath(const QString &path) const { return paths_.indexOf(path); }

signals:
    void currentIndexChanged();
    void countChanged();

private:
    static QString displayName(const QString &path);

    QVector<QString> paths_;      // 完整路径（与 mpv playlist filename 一致）
    QVector<QString> display_;    // 显示名（basename）
    int current_ = -1;
};
