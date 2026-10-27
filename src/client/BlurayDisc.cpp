#include "BlurayDisc.h"
#include "Log.h"

#include <QDir>
#include <QHash>
#include <QFileInfo>

#include <libbluray/bluray.h>

namespace bdisc {

QString formatDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    const qint64 h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    const auto p2 = [](qint64 v) { return QString::number(v).rightJustified(2, '0'); };
    return h > 0 ? QString("%1:%2:%3").arg(h).arg(p2(m), p2(sec))
                 : QString("%1:%2").arg(m).arg(p2(sec));
}

bool isBlurayPath(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isFile())
        return path.endsWith(".iso", Qt::CaseInsensitive);
    if (fi.isDir()) {
        // 允许直接选 BDMV 目录或其上一级
        if (fi.fileName().compare("BDMV", Qt::CaseInsensitive) == 0)
            return true;
        return QFileInfo(path + "/BDMV").isDir();
    }
    return false;
}

BlurayDiscInfo enumerate(const QString &path)
{
    BlurayDiscInfo out;
    // libbluray 自己处理镜像（UDF 层由 libudfread 负责），无需 loop mount/root。
    // keyfile 传 NULL：加密盘的枚举同样可行（mpls/clpi 不加密），播放时才需要密钥。
    BLURAY *bd = bd_open(path.toUtf8().constData(), nullptr);
    if (!bd) {
        out.error = QStringLiteral("libbluray 无法打开（不是蓝光结构或镜像损坏）");
        MLOG_WARN("bluray: open failed: %s", qUtf8Printable(path));
        return out;
    }

    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    out.aacs = di && di->aacs_detected;

    const uint32_t n = bd_get_titles(bd, TITLES_RELEVANT, 0);
    if (n == 0) {
        out.error = QStringLiteral("盘上没有可播放的 title");
        bd_close(bd);
        return out;
    }

    for (uint32_t i = 0; i < n; i++) {
        BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, i, 0);
        if (!ti)
            continue;
        BlurayTitle t;
        t.index = int(i);
        t.playlist = int(ti->playlist);
        t.durationMs = qint64(ti->duration) * 1000 / 90000;   // 90 kHz 时钟
        t.chapterCount = int(ti->mark_count);
        t.clipCount = int(ti->clip_count);
        out.titles.append(t);
        bd_free_title_info(ti);
    }
    bd_close(bd);

    out.ok = !out.titles.isEmpty();
    MLOG_INFO("bluray: %s -> %d titles (aacs=%d)%s",
              qUtf8Printable(QFileInfo(path).fileName()), int(out.titles.size()),
              int(out.aacs), out.ok ? "" : " (enumeration failed)");
    for (const BlurayTitle &t : out.titles)
        MLOG_INFO("bluray:   title %d mpls=%05d %s chapters=%d clips=%d",
                  t.index + 1, t.playlist, qUtf8Printable(formatDuration(t.durationMs)),
                  t.chapterCount, t.clipCount);
    return out;
}

} // namespace bdisc
