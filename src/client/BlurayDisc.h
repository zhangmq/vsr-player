#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

/// 蓝光 ISO / BDMV 目录的标题枚举（libbluray，纯元数据，不解码）。
///
/// 用户 2026-10-26 定的交互：ISO 按"文件夹"处理——打开时把盘上所有 title
/// 作为播放列表条目列出来（**不自动播放**），用户自己挑想看的。
///
/// 只读 mpls/clpi 元数据（未加密），加密盘的枚举同样可用；能否解码取决于
/// libaacs/KEYDB（播放时由 libbluray 报错）。
struct BlurayTitle {
    int index = 0;        ///< libbluray 的 title index（bd://<index> 用）
    int playlist = 0;     ///< mpls 编号（bd://mpls/<n> 用）
    qint64 durationMs = 0;
    int chapterCount = 0;
    int clipCount = 0;
};

struct BlurayDiscInfo {
    bool ok = false;
    bool aacs = false;    ///< 加密标记（枚举不受影响，播放需要密钥）
    QString error;
    QVector<BlurayTitle> titles;
};

// ⚠ 命名空间不能叫 `bluray`：libbluray 的头文件里有 `struct bluray`，
// 同名会造成 "redeclared as different kind of entity"（实测）。
namespace bdisc {

/// 路径是否是蓝光镜像或 BDMV 目录（.iso / .m2ts 目录 / 含 BDMV/ 的目录）。
bool isBlurayPath(const QString &path);

/// 枚举 title（元数据）。失败时 info.error 给出原因。
BlurayDiscInfo enumerate(const QString &path);

/// 把 ms 格式化成 H:MM:SS / MM:SS（与播放列表/OSD 一致）。
QString formatDuration(qint64 ms);

} // namespace bdisc
