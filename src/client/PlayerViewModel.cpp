#include "PlayerViewModel.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include "MpvController.h"
#include "FrucBackend.h"
#include "HdrOutput.h"        // hdr::setUpconvertOptIn / setUpconvertLuminanceOverride
#include "TrueHdrParams.h"    // TrueHDR 参数区间/默认值（客户端单一来源）
#include "Log.h"

#include <QMetaObject>
#include <QVariantMap>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QLocale>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

namespace {

// seek 门控失败兜底（ms）：不可 seek 的流上 mpv 不会发 seeking 翻转，
// 超时后放行下一步，避免方向键永久失效（正常 seek 远快于此）。
constexpr int64_t kSeekGateTimeoutMs = 5000;

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 静音前音量下限：进入静音时音量低于此值则记录此值——
// 退出静音后保证可听（不记录 0/微小的 slider 残值）。
constexpr double kMinUnmuteVolume = 0.05;

QString fmtTime(int64_t ms) {
    int64_t s = ms / 1000;
    return QString("%1:%2:%3")
        .arg(s / 3600,       2, 10, QChar('0'))
        .arg((s % 3600) / 60, 2, 10, QChar('0'))
        .arg(s % 60,         2, 10, QChar('0'));
}

bool isSubtitleFile(const QString &p) {
    const QString ext = p.section('.', -1).toLower();
    static const char *subs[] = {"srt", "ass", "ssa", "vtt", "sub", "sbv"};
    for (const char *s : subs)
        if (ext == QLatin1String(s)) return true;
    return false;
}

QString toLocalPath(const QString &s) {
    QUrl u(s);
    return u.isLocalFile() ? u.toLocalFile() : s;   // 网络流原样传 mpv
}

const char *qualityName(int v) {
    switch (v) {
    case 1:  return QT_TR_NOOP("Low");
    case 2:  return QT_TR_NOOP("Medium");
    case 3:  return QT_TR_NOOP("High");
    case 4:  return QT_TR_NOOP("Ultra");
    default: return QT_TR_NOOP("off");
    }
}

const char *denoiseName(int v) {
    switch (v) {
    case 8:  return QT_TR_NOOP("Low");
    case 9:  return QT_TR_NOOP("Medium");
    case 10: return QT_TR_NOOP("High");
    case 11: return QT_TR_NOOP("Ultra");
    default: return QT_TR_NOOP("off");
    }
}

}  // namespace

PlayerViewModel::PlayerViewModel(QObject *parent) : QObject(parent) {
    // TrueHDR 总开关的**默认值** = env `VSR_UPHDR`（无持久化设置时生效）；
    // loadSettings() 读到设置后以设置为准（benchmark 不读设置 → 保持 env 语义）。
    persistUpHdrEnabled_ = hdr::upconvertOptedIn();
}

// ── attach: register mpv property observers ────────────────────────────
//
// 线程模型（关键，防 untimed 死锁）：
//  主线程 = 渲染线程（mpv_render_context_render 只在主线程调用）；
//  vo_thread 的 flip_page 持 render ctx lock 等 render 消费（200ms
//  超时，有界）。若主线程同步调用 client API（mpv_get_property 等），
//  会持 client lock 等 core lock；播放循环（持 core lock）经
//  vo_thread flip_page 等主线程渲染 → core lock 互等 → 渲染循环
//  停摆、播放停滞（untimed 实测卡死）。
//  因此：**属性读取全部在事件线程执行**（阻塞无碍），值随
//  QMetaObject::invokeMethod 的 lambda 参数传给主线程——主线程状态
//  更新方法绝不调用 mpv 任何 API。

void PlayerViewModel::attach(MpvController *mpv) {
    mpv_ = mpv;
    // OSD 拉取推送（事件线程 idle 回调，~100ms）——注册须在 startEvents 前。
    // benchmark 与正常播放共用同一机制（osdVisible 控制）；主线程调 mpv
    // API 更新 osd-msg1 会与 untimed 渲染死锁（实测卡死），故推送在事件线程。
    mpv->setIdlePollCallback([this] { osdPoll(); });
    auto post = [this](auto fn) {
        QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
    };

    mpv_->observeProperty("pause", MPV_FORMAT_FLAG, [this, post] {
        bool p = mpv_->propertyFlag("pause");
        int64_t drops = mpv_->propertyInt64("frame-drop-count");  // 事件线程读
        post([this, p, drops] {
            // 启动 idle（列表已排队、媒体未载入）时 mpv 的 pause 初值为
            // false——没有活动文件就不算"播放中"，否则 UI 按钮显示成暂停
            // 图标。有文件时语义不变（playing = !pause）。
            updatePlaying(!p && fileLoaded_);
            if (p != pausePrev_) {
                pausePrev_ = p;
                resetSegmentCounters(drops);   // pause 值翻转 → 段重置
            }
        });
    });

    mpv_->observeProperty("time-pos", MPV_FORMAT_DOUBLE, [this, post] {
        double t = mpv_->propertyDouble("time-pos");
        double d = mpv_->propertyDouble("duration");
        post([this, t, d] {
            updateTime(t < 0 ? 0 : (int64_t)(t * 1000),
                       d < 0 ? 0 : (int64_t)(d * 1000));
        });
    });

    mpv_->observeProperty("volume", MPV_FORMAT_DOUBLE, [this, post] {
        double v = mpv_->propertyDouble("volume") / 100.0;
        post([this, v] { updateVolume(v); });
    });

    // 无 mute 状态位：volume 观察回写即推导静音（volume==0）——
    // mpv `mute` 属性不再使用，toggleMute 走音量 0/恢复。

    mpv_->observeProperty("speed", MPV_FORMAT_DOUBLE, [this, post] {
        double s = mpv_->propertyDouble("speed");
        post([this, s] { updateSpeed(s); });
    });

    // hwdec-active 在 mpv 0.41 已移除（读不到 → UI 永远显示"软解"），
    // 用 hwdec-current 替代（与 main.cpp benchmark OSD 同口径）。
    mpv_->observeProperty("hwdec-current", MPV_FORMAT_STRING, [this, post] {
        std::string s = mpv_->propertyString("hwdec-current");
        post([this, s] {
            updateHwDecoding(!s.empty() && s != "no");
            refreshHwDecodeAvailability();
            if (hwdecName_ != QString::fromStdString(s)) {
                hwdecName_ = QString::fromStdString(s);
                emit osdDataChanged();
            }
        });
    });

    // path 仅记录最后播放路径（idle 重播用），不做状态判定——曲目
    // 切换时 mpv 会临时清空 filename（loadfile.c start_play →
    // filename=NULL，path 属性不可用），据此判定停止会误触发。
    mpv_->observeProperty("path", MPV_FORMAT_STRING, [this, post] {
        std::string p = mpv_->propertyString("path");
        post([this, p] {
            // ⚠ 这个 lambda 在 GUI 线程执行，**绝不能**再同步调 mpv API：
            // 软解 + vo=libmpv 时 DR 帧分配靠 GUI 线程在
            // mpv_render_context_update() 里服务（vo_libmpv.c），GUI 线程
            // 一旦阻塞在 mpv_get_property（等 core 的 dispatch 锁），解码
            // 线程拿不到 DR 帧 → core 卡在 send_packet → 双向死锁。
            // 实测：v0.4.3 的 refreshCanDeleteCurrentFile() 在这里重读 path，
            // 软解启动 1080p h264/hevc 必挂（窗口无响应、只能杀进程）。
            // path 观察值 p 已经够用，直接传下去。
            const QString path = QString::fromStdString(p);
            if (!path.isEmpty())
                lastPath_ = path;
            refreshCanDeleteCurrentFile(path);   // 换文件 → "能否删除"跟着变
        });
    });

    // idle-active（stop_play == PT_STOP）才是"真正停止"的信号：
    // 停止命令 / 播完且无后续条目时置真；曲目切换期间保持假。
    mpv_->observeProperty("idle-active", MPV_FORMAT_FLAG, [this, post] {
        bool idle = mpv_->propertyFlag("idle-active");
        int64_t drops = mpv_->propertyInt64("frame-drop-count");
        post([this, idle, drops] {
            if (fileLoaded_ != !idle) emit hasMediaChanged();
            fileLoaded_ = !idle;
            if (idle) {
                updatePlaying(false);
                resetSegmentCounters(drops);   // 停止/播完 → 段重置
            }
        });
    });

    mpv_->observeProperty("filename", MPV_FORMAT_STRING, [this, post] {
        QString f = QString::fromStdString(mpv_->propertyString("filename"));
        post([this, f] { updateVideoInfo(f); });
    });

    mpv_->observeProperty("playlist", MPV_FORMAT_NODE, [this, post] {
        QStringList files;
        int cur = -1;
        mpv_node node;
        if (mpv_get_property(mpv_->handle(), "playlist", MPV_FORMAT_NODE,
                             &node) >= 0) {
            if (node.format == MPV_FORMAT_NODE_ARRAY) {
                for (int i = 0; i < node.u.list->num; i++) {
                    mpv_node *entry = &node.u.list->values[i];
                    if (entry->format != MPV_FORMAT_NODE_MAP)
                        continue;
                    QString fn;
                    for (int j = 0; j < entry->u.list->num; j++) {
                        const char *k = entry->u.list->keys[j];
                        mpv_node *v = &entry->u.list->values[j];
                        if (!strcmp(k, "filename") && v->format == MPV_FORMAT_STRING)
                            fn = v->u.string;
                        else if (!strcmp(k, "current") && v->format == MPV_FORMAT_FLAG
                                 && v->u.flag)
                            cur = i;
                    }
                    files.append(fn);
                }
            }
            mpv_free_node_contents(&node);
        }
        // 全量快照 → PlaylistModel 前缀 diff 增量生效（切歌只刷
        // current 行高亮，不重建列表——QStringList 属性全量 reset
        // 是大列表卡顿主因）
        post([this, files, cur] {
            // 缓存的 current 索引（= mpv playlist-pos，-1 = 无当前条目）。
            // "移除当前条目"用它，避免在 GUI 线程同步读 playlist-pos
            //（同 path 观察器注释：同步 mpv API 会和 DR 帧分配互锁）。
            playlistPos_ = cur;
            // 列表**变空**（移除最后一个条目 / 清空播放列表）→ 忘掉重播目标：
            // 否则 play() 的 !fileLoaded_ 分支会走 resumeLastPath()，把刚被
            // 移除的文件 append-play **重新加回列表**（用户实测："删了当前
            // 文件，画面只是停住，再点播放又回来了"）。
            if (files.isEmpty()) {
                lastPath_.clear();
                idlePlayIndex_ = -1;
                if (!videoInfo_.isEmpty()) {   // 标题别停在已移除的文件上
                    videoInfo_.clear();
                    emit videoInfoChanged();
                }
            }
            // 启动 idle：列表只有排队条目、mpv 无 current（cur<0）→ 用记录的
            // 目标索引高亮"按播放将载入的那一条"（否则列表看起来没有选中项）。
            // 真正载入后 mpv 报 cur>=0，自然回到 mpv 的事实源。
            int shown = cur;
            if (shown < 0 && idlePlayIndex_ >= 0 && idlePlayIndex_ < files.size())
                shown = idlePlayIndex_;
            playlistModel_.setSnapshot(files, shown);
            // 持久化上次播放列表（启动 restorePlaylist 用；清空也写）
            saveSettings("playlist", files);
            saveSettings("playlistCurrent", shown);
        });
    });

    mpv_->observeProperty("loop-file", MPV_FORMAT_STRING, [this] {
        updateLoopModeFromEventThread();
    });
    mpv_->observeProperty("loop-playlist", MPV_FORMAT_STRING, [this] {
        updateLoopModeFromEventThread();
    });

    // ── OSD 数据源观察器（全部事件线程读，post 主线程）─────────────
    // postData 与上方 post 并存：post 更新播放状态，postData 更新 OSD
    // 数据源属性（osdDataChanged 信号）。mpv 属性用 MPV_FORMAT_NODE 时
    // 直接 mpv_get_property 获取（参照 playlist 观察器）。
    auto postData = [this](auto fn) {
        QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
    };

    // 实际解码器（软解：libdav1d/h264/hevc 等；硬解：codec 名如 av1，
    // 与 hwdec-current 组合即完整解码器信息）
    mpv_->observeProperty("current-tracks/video/decoder", MPV_FORMAT_STRING,
                          [this, postData] {
        std::string d = mpv_->propertyString("current-tracks/video/decoder");
        postData([this, d] {
            if (decoderName_ != QString::fromStdString(d)) {
                decoderName_ = QString::fromStdString(d);
                refreshHwDecodeAvailability();
                emit osdDataChanged();
            }
        });
    });

    mpv_->observeProperty("video-params", MPV_FORMAT_NODE, [this, postData] {
        // 解码尺寸（VSR 前）与像素格式/色深
        mpv_node node;
        if (mpv_get_property(mpv_->handle(), "video-params", MPV_FORMAT_NODE,
                             &node) >= 0) {
            int w = 0, h = 0, bd = 0;
            std::string pix, hwPix, gamma;
            // HDR10 静态元数据（合成器做 tone mapping 时假设的"内容峰值"依据，
            // 见 hdr::ContentMeta / WaylandColorFeedback.h）：min/max-luma 是
            // ST 2086 mastering 范围，max-cll/max-fall 是 CTA-861 内容光级。
            double minLuma = 0, maxLuma = 0, maxCll = 0, maxFall = 0;
            auto num = [](const mpv_node *v, double *out) {
                if (v->format == MPV_FORMAT_DOUBLE) { *out = v->u.double_; return true; }
                if (v->format == MPV_FORMAT_INT64)  { *out = double(v->u.int64); return true; }
                return false;
            };
            if (node.format == MPV_FORMAT_NODE_MAP) {
                for (int j = 0; j < node.u.list->num; j++) {
                    const char *k = node.u.list->keys[j];
                    mpv_node *v = &node.u.list->values[j];
                    if (!strcmp(k, "w") && v->format == MPV_FORMAT_INT64)
                        w = (int)v->u.int64;
                    else if (!strcmp(k, "h") && v->format == MPV_FORMAT_INT64)
                        h = (int)v->u.int64;
                    else if (!strcmp(k, "pixelformat") &&
                             v->format == MPV_FORMAT_STRING)
                        pix = v->u.string;
                    else if (!strcmp(k, "hw-pixelformat") &&
                             v->format == MPV_FORMAT_STRING)
                        hwPix = v->u.string;
                    else if (!strcmp(k, "gamma") &&
                             v->format == MPV_FORMAT_STRING)
                        gamma = v->u.string;
                    else if (!strcmp(k, "min-luma"))   num(v, &minLuma);
                    else if (!strcmp(k, "max-luma"))   num(v, &maxLuma);
                    else if (!strcmp(k, "max-cll"))    num(v, &maxCll);
                    else if (!strcmp(k, "max-fall"))   num(v, &maxFall);
                }
                // 色深：hw-pixelformat 优先（硬解时 pixelformat 是
                // cuda/nv12 包装名），fallback pixelformat。解析 'p' 后
                // 跟的数字（"p10"/"p12"…），注意 NV12 10bit 是 "p010"
                //（无 "p10" 子串，子串匹配会漏）。无 'p' 数字即 8bit。
                const std::string &fmt = hwPix.empty() ? pix : hwPix;
                for (size_t i = 0; i + 1 < fmt.size() && bd == 0; i++) {
                    if (fmt[i] != 'p') continue;
                    int d = 0;
                    size_t j = i + 1;
                    while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') {
                        d = d * 10 + (fmt[j] - '0');
                        j++;
                    }
                    if (j > i + 1 && d >= 8 && d <= 16) bd = d;
                }
                if (bd == 0 && !fmt.empty()) bd = 8;   // 无后缀：8bit
            }
            mpv_free_node_contents(&node);
            // 内容侧 HDR 判据：gamma = pq / hlg（HDR10/HLG 源）。BT.2020 容器 +
            // sig-peak>1 也可作弱信号，但常见 HDR 源都会带 pq/hlg → 只用它。
            const bool hdr = (gamma == "pq" || gamma == "hlg");
            HdrMeta meta;
            meta.valid = hdr && maxLuma > minLuma && maxLuma > 0;
            meta.minMastering = minLuma;
            meta.maxMastering = maxLuma;
            meta.maxCll = maxCll;
            meta.maxFall = maxFall;
            postData([this, w, h, pix, hwPix, bd, hdr, gamma, meta] {
                if (videoWidth_ != w || videoHeight_ != h) {
                    videoWidth_.store(w); videoHeight_.store(h);
                    emit osdDataChanged();
                }
                if (decoderPixelFormat_ != QString::fromStdString(pix)) {
                    decoderPixelFormat_ = QString::fromStdString(pix);
                    emit osdDataChanged();
                }
                if (hwPixelFormat_ != QString::fromStdString(hwPix)) {
                    hwPixelFormat_ = QString::fromStdString(hwPix);
                    emit osdDataChanged();
                }
                if (videoBitDepth_.load() != bd) {
                    videoBitDepth_.store(bd);
                    emit osdDataChanged();
                }
                if (videoHdr_ != hdr) {
                    videoHdr_ = hdr;
                    MLOG_INFO("video-params: hdr=%d（gamma=%s）", int(hdr), gamma.c_str());
                    emit videoHdrChanged();
                }
                if (hdrMeta_.valid != meta.valid ||
                    hdrMeta_.minMastering != meta.minMastering ||
                    hdrMeta_.maxMastering != meta.maxMastering ||
                    hdrMeta_.maxCll != meta.maxCll ||
                    hdrMeta_.maxFall != meta.maxFall) {
                    hdrMeta_ = meta;
                    MLOG_INFO("video-params: HDR10 metadata mastering %.4f-%.0f nit, "
                              "max_cll=%.0f, max_fall=%.0f (%s)",
                              meta.minMastering, meta.maxMastering, meta.maxCll,
                              meta.maxFall,
                              meta.valid ? "valid" : "absent -> compositor assumes 10000 nit");
                    emit videoHdrMetaChanged();
                }
            });
        }
    });

    // video-format = current-tracks/video/codec（解码 codec 名，如
    // av1/hevc/h264）。不用 current-demuxer/codec——demuxer 属性只有
    // 容器格式名（mkv 等），/codec 子属性不存在，恒返回不可用。
    mpv_->observeProperty("video-format", MPV_FORMAT_STRING, [this, postData] {
        std::string c = mpv_->propertyString("video-format");
        postData([this, c] {
            if (videoCodec_ != QString::fromStdString(c)) {
                videoCodec_ = QString::fromStdString(c);
                emit osdDataChanged();
            }
        });
    });

    mpv_->observeProperty("container-fps", MPV_FORMAT_DOUBLE, [this, postData] {
        double f = mpv_->propertyDouble("container-fps");
        postData([this, f] { if (videoFps_.load() != f) { videoFps_.store(f); emit osdDataChanged(); } });
    });

    mpv_->observeProperty("estimated-frame-count", MPV_FORMAT_INT64, [this, postData] {
        int64_t n = mpv_->propertyInt64("estimated-frame-count");
        postData([this, n] { if (decodedFrames_.load() != n) { decodedFrames_.store(n); emit osdDataChanged(); } });
    });

    mpv_->observeProperty("video-out-params", MPV_FORMAT_NODE, [this, postData] {
        // video-out-params/w×h = filter 链输出帧像素尺寸（VSR 后；VSR off = 源尺寸）
        mpv_node node;
        if (mpv_get_property(mpv_->handle(), "video-out-params", MPV_FORMAT_NODE,
                             &node) >= 0) {
            int w = 0, h = 0;
            if (node.format == MPV_FORMAT_NODE_MAP) {
                for (int j = 0; j < node.u.list->num; j++) {
                    const char *k = node.u.list->keys[j];
                    mpv_node *v = &node.u.list->values[j];
                    if (!strcmp(k, "w") && v->format == MPV_FORMAT_INT64)
                        w = (int)v->u.int64;
                    else if (!strcmp(k, "h") && v->format == MPV_FORMAT_INT64)
                        h = (int)v->u.int64;
                }
            }
            mpv_free_node_contents(&node);
            postData([this, w, h] {
                if (renderWidth_ != w || renderHeight_ != h) {
                    renderWidth_.store(w); renderHeight_.store(h);
                    emit osdDataChanged();
                }
            });
        }
    });

    mpv_->observeProperty("osd-dimensions", MPV_FORMAT_NODE, [this, postData] {
        // 视频视口物理尺寸 = (w-ml-mr)×(h-mt-mb)（扣除 letterbox 边距）
        mpv_node node;
        if (mpv_get_property(mpv_->handle(), "osd-dimensions", MPV_FORMAT_NODE,
                             &node) >= 0) {
            int w = 0, h = 0, ml = 0, mr = 0, mt = 0, mb = 0;
            if (node.format == MPV_FORMAT_NODE_MAP) {
                for (int j = 0; j < node.u.list->num; j++) {
                    const char *k = node.u.list->keys[j];
                    mpv_node *v = &node.u.list->values[j];
                    if (v->format != MPV_FORMAT_INT64) continue;
                    int val = (int)v->u.int64;
                    if (!strcmp(k, "w")) w = val;
                    else if (!strcmp(k, "h")) h = val;
                    else if (!strcmp(k, "ml")) ml = val;
                    else if (!strcmp(k, "mr")) mr = val;
                    else if (!strcmp(k, "mt")) mt = val;
                    else if (!strcmp(k, "mb")) mb = val;
                }
            }
            mpv_free_node_contents(&node);
            int vw = w - ml - mr, vh = h - mt - mb;
            postData([this, vw, vh] {
                if (viewportWidth_ != vw || viewportHeight_ != vh) {
                    viewportWidth_.store(vw); viewportHeight_.store(vh);
                    emit osdDataChanged();
                }
            });
        }
    });

    mpv_->observeProperty("frame-drop-count", MPV_FORMAT_INT64, [this, postData] {
        int64_t n = mpv_->propertyInt64("frame-drop-count");
        droppedCache_.store(n);                  // 总是缓存最新值（OSD 重开时 catch-up）
        if (!osdVisible_.load()) return;         // 节流：不可见不 post
        postData([this, n] {
            if (droppedCache_.load() != n) return;  // 值已变则丢弃（防乱序）
            emit osdDataChanged();
        });
    });

    mpv_->observeProperty("audio-params/samplerate", MPV_FORMAT_INT64, [this, postData] {
        int sr = (int)mpv_->propertyInt64("audio-params/samplerate");
        postData([this, sr] { if (audioSampleRate_.load() != sr) { audioSampleRate_.store(sr); emit osdDataChanged(); } });
    });
    mpv_->observeProperty("audio-params/channel-count", MPV_FORMAT_INT64, [this, postData] {
        int ch = (int)mpv_->propertyInt64("audio-params/channel-count");
        postData([this, ch] { if (audioChannels_.load() != ch) { audioChannels_.store(ch); emit osdDataChanged(); } });
    });

    mpv_->observeProperty("seeking", MPV_FORMAT_FLAG, [this, postData] {
        bool seeking = mpv_->propertyFlag("seeking");
        // 注意：连续快进/快退的门控**不在这里**做。libmpv 的属性观察对值去重
        // （连续 seek 时 true/false 互相抵消，最后一条完成通知可能不送达），
        // 完成信号用 MPV_EVENT_PLAYBACK_RESTART（见 notePlaybackRestart）。
        int64_t drops = mpv_->propertyInt64("frame-drop-count");
        postData([this, seeking, drops] {
            if (!seeking && seekingPrev_)   // true→false：seek 完成
                resetSegmentCounters(drops);
            seekingPrev_ = seeking;
        });
    });

    // ── 轨道列表（音轨/字幕轨/视频轨，TracksPopup 数据源）────────
    // 与 playlist 观察器同构：事件线程解析 mpv_node → QVariantList →
    // Queued 转发主线程。变化低频（加载/切换），延迟可接受。
    mpv_->observeProperty("track-list", MPV_FORMAT_NODE, [this, post] {
        QVariantList tracks;
        mpv_node node;
        if (mpv_get_property(mpv_->handle(), "track-list", MPV_FORMAT_NODE,
                             &node) >= 0) {
            if (node.format == MPV_FORMAT_NODE_ARRAY) {
                for (int i = 0; i < node.u.list->num; i++) {
                    mpv_node *e = &node.u.list->values[i];
                    if (e->format != MPV_FORMAT_NODE_MAP) continue;
                    QVariantMap t;
                    t["type"] = QStringLiteral("video");
                    t["id"] = qlonglong(0);
                    t["lang"] = QString();
                    t["title"] = QString();
                    t["selected"] = false;
                    t["external"] = false;
                    t["filename"] = QString();
                    for (int j = 0; j < e->u.list->num; j++) {
                        const char *k = e->u.list->keys[j];
                        mpv_node *v = &e->u.list->values[j];
                        if (!strcmp(k, "type") && v->format == MPV_FORMAT_STRING)
                            t["type"] = QString::fromUtf8(v->u.string);
                        else if (!strcmp(k, "id") && v->format == MPV_FORMAT_INT64)
                            t["id"] = qlonglong(v->u.int64);
                        else if (!strcmp(k, "lang") && v->format == MPV_FORMAT_STRING)
                            t["lang"] = QString::fromUtf8(v->u.string);
                        else if (!strcmp(k, "title") && v->format == MPV_FORMAT_STRING)
                            t["title"] = QString::fromUtf8(v->u.string);
                        else if (!strcmp(k, "selected") && v->format == MPV_FORMAT_FLAG)
                            t["selected"] = v->u.flag != 0;   // bool 而非 int（QML 严格相等陷阱）
                        else if (!strcmp(k, "external") && v->format == MPV_FORMAT_FLAG)
                            t["external"] = v->u.flag != 0;
                        else if (!strcmp(k, "external-filename") && v->format == MPV_FORMAT_STRING)
                            t["filename"] = QString::fromUtf8(v->u.string);   // 外部轨完整路径（mpv 字段名）
                    }
                    tracks.append(t);
                }
            }
            mpv_free_node_contents(&node);
        }
        post([this, tracks] {
            if (trackList_ != tracks) { trackList_ = tracks; emit trackListChanged(); }
        });
    });

    // 字幕可见性 / 延迟（初始值随 observe 立即送达；setter 全异步，
    // 主线程零 mpv 读——遵循 attach 线程模型注释）
    mpv_->observeProperty("sub-visibility", MPV_FORMAT_FLAG, [this, post] {
        bool v = mpv_->propertyFlag("sub-visibility");
        post([this, v] { if (subVisible_ != v) { subVisible_ = v; emit subVisibleChanged(); } });
    });
    mpv_->observeProperty("sub-delay", MPV_FORMAT_DOUBLE, [this, post] {
        double d = mpv_->propertyDouble("sub-delay");
        post([this, d] { if (subDelay_ != d) { subDelay_ = d; emit subDelayChanged(); } });
    });
}

void PlayerViewModel::initVsr(const std::string &scale,
                              const std::string &quality,
                              const std::string &denoise) {
    // 空 = CLI 未显式指定 → 用持久化值（loadSettings 已读）；显式但
    // 无法解析 → 回退默认（auto / high / off）。
    double s;
    if (scale.empty()) s = persistScale_;
    else if (!parseScale(scale, &s)) s = 0.0;
    scale_ = s;
    int q;
    if (quality.empty()) q = persistQuality_;
    else if (!parseQuality(quality, &q)) q = 3;
    quality_ = q;
    int d;
    if (denoise.empty()) d = persistDenoise_;
    else if (!parseDenoise(denoise, &d)) d = -1;
    denoise_ = d;
}

void PlayerViewModel::initFruc(const std::string &fruc, bool benchmark) {
    // 单一字段 frucFps_（VFG 分支 -1 off | 2/3/4 倍率；RIFE 分支 -1 off |
    // 40/48/60 目标帧率 | 2/3/4 benchmark 倍率）——benchmark 与正常模式共用
    // 一套状态（OSD/vf 构造不分叉）。合法值见 FrucBackend.h。
    int v;
    if (fruc::parseCli(fruc, &v)) {
        // RIFE：2/3/4 是 **benchmark 专用**倍率，会覆盖目标帧率（vf_rife.c:63），
        // 并让界面档位失效（实测：以 --fruc 2 起步后，界面/RPC 选 40/48/60/off
        // 全都改不动实际输出）。非 benchmark 下一律**当作没传**（忽略 + 回落
        // 持久化档位），避免进入"控件看起来有效、实际无效"的状态。
        // VFG 分支里 2/3/4 就是正常倍率档位，不受此限。
#ifndef FRUC_BACKEND_VFG
        const bool benchmark_only = (v == 2 || v == 3 || v == 4);
#else
        const bool benchmark_only = false;
#endif
        if (benchmark || !benchmark_only) {
            frucFps_ = v;   // 含 "off" → -1（显式 off 本次播放关闭，不落持久化）
            return;
        }
        MLOG_WARN("--fruc %d is benchmark-only for the RIFE backend (2/3/4 are "
                  "multipliers): ignored, using the saved setting", v);
    } else if (!fruc.empty() && fruc::kIsVfg) {
        // VFG：40/48/60 是 RIFE 的目标帧率档（VFG 只有倍率档）——同样**当没传**
        // 处理（回落持久化档位），与上面 RIFE 侧对 2/3/4 的做法对称。
        MLOG_WARN("--fruc %s is RIFE-only (this is the VFG backend, expect %s): "
                  "ignored, using the saved setting",
                  fruc.c_str(), fruc::cliValuesHelp().c_str());
    }
    frucFps_ = benchmark ? -1 : persistFruc_;
}

void PlayerViewModel::initFrucOrder(const std::string &order) {
    if (!fruc::kOrderSelectable) {
        // RIFE 链序固定（hwup → rife → vsr → VO，插帧在源分辨率）：选项
        // 接受但无效果——显式提示，避免"设了没反应"被当成缺陷。
        if (!order.empty())
            MLOG_WARN("--fruc-order %s ignored: not selectable with the RIFE backend",
                      order.c_str());
        return;
    }
    if (order.empty() || order == fruc::kOrderDefault) {
        frucOrderPre_ = false;
        return;
    }
    if (order == "pre") {
        frucOrderPre_ = true;
        MLOG_INFO("FRUC vf order: pre (vfg before vsr)");
        return;
    }
    MLOG_WARN("--fruc-order %s invalid (expect pre|%s): using %s",
              order.c_str(), fruc::kOrderDefault, fruc::kOrderDefault);
}

void PlayerViewModel::initFrucQuality(const std::string &quality) {
    if (!fruc::kModeSelectable) {
        // RIFE 无质量档（mode 是 VFG VideoFrameGeneration 的引擎参数）：
        // 选项接受但无效果——显式提示，避免"设了没反应"被当成缺陷
        //（照 --fruc-order 的既有处理）。
        if (!quality.empty())
            MLOG_WARN("--fruc-quality %s ignored: not selectable with the RIFE backend",
                      quality.c_str());
        return;
    }
    if (quality.empty()) {
        // 空 = CLI 未显式指定 → 持久化值（loadSettings 已清洗）。
        frucQuality_ = persistFrucQuality_;
        return;
    }
    int v;
    if (!fruc::parseMode(quality, &v)) {
        MLOG_WARN("--fruc-quality %s invalid (expect %s): using %s",
                  quality.c_str(), fruc::modeValuesHelp().c_str(),
                  fruc::modeName(fruc::kModeDefault));
        frucQuality_ = fruc::kModeDefault;
        return;
    }
    frucQuality_ = v;
    MLOG_INFO("FRUC quality: mode=%s (%d)", fruc::modeName(v), v);
}

void PlayerViewModel::initFrucMaxFps(const std::string &max_fps) {
    if (!fruc::kMaxFpsSelectable) {
        // 输出帧率上限是 VFG 倍率语义的补丁：RIFE 的 --fruc 本身就是目标
        // 输出帧率（40/48/60），没有"倍率把帧率顶过头"的问题 → 接受但忽略。
        if (!max_fps.empty())
            MLOG_WARN("--fruc-max-fps %s ignored: VFG-only (this is the RIFE backend)",
                      max_fps.c_str());
        return;
    }
    if (max_fps.empty()) {
        // 空 = CLI 未显式指定 → 持久化值（loadSettings 已清洗）。
        frucMaxFps_ = persistFrucMaxFps_;
        return;
    }
    char *end = nullptr;
    long v = strtol(max_fps.c_str(), &end, 10);
    if (end == max_fps.c_str() || *end != '\0' || !fruc::isValidMaxFps((int)v)) {
        MLOG_WARN("--fruc-max-fps %s invalid (expect %s): using %d",
                  max_fps.c_str(), fruc::maxFpsValuesHelp().c_str(),
                  fruc::kMaxFpsDefault);
        frucMaxFps_ = fruc::kMaxFpsDefault;
        return;
    }
    frucMaxFps_ = (int)v;
    MLOG_INFO("FRUC max output fps: %d", (int)v);
}

bool PlayerViewModel::parseScale(const std::string &s, double *out) {
    if (s == "off")      { *out = -1.0; return true; }
    if (s == "auto" || s.empty()) { *out = 0.0; return true; }
    if (s == "4/3")      { *out = 4.0 / 3.0; return true; }
    if (s == "1.5")      { *out = 1.5; return true; }
    if (s == "2")        { *out = 2.0; return true; }
    if (s == "3")        { *out = 3.0; return true; }
    if (s == "4")        { *out = 4.0; return true; }
    return false;
}

bool PlayerViewModel::parseQuality(const std::string &s, int *out) {
    if (s == "low")    { *out = 1; return true; }
    if (s == "medium") { *out = 2; return true; }
    if (s == "high")   { *out = 3; return true; }
    if (s == "ultra")  { *out = 4; return true; }
    return false;
}

bool PlayerViewModel::parseDenoise(const std::string &s, int *out) {
    if (s == "off")    { *out = -1; return true; }
    if (s == "low")    { *out = 8;  return true; }
    if (s == "medium") { *out = 9;  return true; }
    if (s == "high")   { *out = 10; return true; }
    if (s == "ultra")  { *out = 11; return true; }
    return false;
}

void PlayerViewModel::setGpuName(const QString &name) { gpuName_ = name; }

// ── 持久化（QSettings；键名与 Task 4 main.cpp 几何恢复一致约定）───────

void PlayerViewModel::saveSettings(const char *key, const QVariant &value) {
    if (!settingsEnabled_) return;
    settings_.setValue(key, value);
}

void PlayerViewModel::loadSettings() {
    settingsEnabled_ = true;
    persistScale_ = settings_.value("scale", 0.0).toDouble();
    if (persistScale_ < -1.0 || persistScale_ > 4.0 || persistScale_ == 1.0)
        persistScale_ = 0.0;   // 非法值（手改配置）→ auto
    persistQuality_ = settings_.value("quality", 3).toInt();
    persistDenoise_ = settings_.value("denoiseQuality", -1).toInt();
    persistFruc_ = settings_.value("frucFps", -1).toInt();
    if (!fruc::isValidPersisted(persistFruc_))
        persistFruc_ = -1;   // 非法值（手改配置/上一后端残留）→ off
    persistFrucQuality_ = settings_.value("frucQuality", fruc::kModeDefault).toInt();
    if (!fruc::isValidMode(persistFrucQuality_))
        persistFrucQuality_ = fruc::kModeDefault;   // 非法值（手改配置）→ medium
    persistFrucMaxFps_ = settings_.value("frucMaxFps", fruc::kMaxFpsDefault).toInt();
    if (!fruc::isValidMaxFps(persistFrucMaxFps_))
        persistFrucMaxFps_ = fruc::kMaxFpsDefault;  // 非法值（手改配置）→ 120

    // ── TrueHDR（SDR→HDR10 上变换）参数 ────────────────────────────────
    // 区间/默认值单一来源 = TrueHdrParams.h（= SDK 文档串）；手改配置越界 →
    // 夹回合法区间（不整份丢弃，避免一个坏值让全部参数失效）。
    using namespace truehdr_params;
    persistUpHdrEnabled_ = settings_.value("upHdrEnabled", persistUpHdrEnabled_).toBool();
    persistUpHdrLumOverride_ = settings_.value("upHdrLumOverride", false).toBool();
    persistUpHdrLuminance_ = clampInt(settings_.value("upHdrLuminance", kLuminanceDefault).toInt(),
                                      kLuminanceMin, kLuminanceMax);
    persistUpHdrSaturation_ = clampInt(settings_.value("upHdrSaturation", kSaturationDefault).toInt(),
                                       kSaturationMin, kSaturationMax);
    persistUpHdrContrast_ = clampInt(settings_.value("upHdrContrast", kContrastDefault).toInt(),
                                     kContrastMin, kContrastMax);
    persistUpHdrMiddleGray_ = clampInt(settings_.value("upHdrMiddleGray", kMiddleGrayDefault).toInt(),
                                       kMiddleGrayMin, kMiddleGrayMax);
    persistUpHdrDebanding_ = settings_.value("upHdrDebanding", kDebandingDefault).toBool();

    // ── 反交错（0=Off / 1=Auto / 2=Force）─────────────────────────────
    // 手改越界 → 夹回 0..2（不整份丢弃）。CLI `-- --deinterlace=…` 优先，
    // 见 initDeinterlace()（在 applyPlaybackSettings 之前调用）。
    persistDeintMode_ = clampInt(settings_.value("deintMode", 0).toInt(), 0, 2);

    // ── 旋转（0=Off / 90=顺时针 / 270=逆时针 90°）─────────────────────
    // 非法值（含旧的 180）→ 0；vfOption() 在 loadSettings 之后取当前值拼链。
    int rot = settings_.value("rotMode", 0).toInt();
    if (rot != 90 && rot != 270)
        rot = 0;
    persistRotMode_ = rot;
    rotMode_.store(rot);
}

void PlayerViewModel::applyPlaybackSettings() {
    if (!mpv_) return;
    setVolume(settings_.value("volume", 1.0).toDouble());
    setSpeed(settings_.value("speed", 1.0).toDouble());
    setLoopMode(settings_.value("loopMode", 0).toInt());
    setAspect(settings_.value("aspect", QStringLiteral("no")).toString());
    applyDeinterlace();   // mpv 选项（非 vf）——链重建后也由 FILE_LOADED 重放

    // 解码偏好初值 = mpv 当前的 `hwdec` 选项（含 `-- --hwdec=no` 透传）。
    // 之后由 toggleHwaccel() 翻转；**实际**状态另走 hwdec-current 观察器。
    if (hwdecInit_.empty()) {
        hwdecInit_ = mpv_->propertyString("hwdec");
        bool hwOn = !hwdecInit_.empty() && hwdecInit_ != "no";
        if (hwDecodeEnabled_.load() != hwOn) {
            hwDecodeEnabled_.store(hwOn);
            emit hwDecodeEnabledChanged();
        }
    }
    refreshHwDecodeAvailability();
}

void PlayerViewModel::restorePlaylist() {
    if (!mpv_) return;
    int cur = 0;
    QStringList files = restoredFiles(&cur);
    if (files.isEmpty()) return;
    // 首条 replace 开始播放，其余 append 排队；最后定位上次条目。
    // loadfile 全部同步命令（mpv 启动阶段无 core-lock 竞争）。
    mpv_->commandV({"loadfile", files[0].toUtf8().constData(), nullptr});
    for (int i = 1; i < files.size(); i++)
        mpv_->commandV({"loadfile", files[i].toUtf8().constData(), "append", nullptr});
    if (cur != 0)
        mpv_->commandV({"playlist-play-index", std::to_string(cur).c_str(), nullptr});
}

// 启动 idle（用户要求）：只把列表排进 mpv，**不载入媒体**。
// `loadfile <p> append` 的 action.play=false → 列表没有 current 条目
//（third_party/mpv/player/command.c cmd_loadfile 的启动条件 =
// LOAD_TYPE_REPLACE 或 (action.play && !playlist->current)）→ 不解码、
// 不建滤镜链、不 Load VSR/VFG 引擎、不做 watch_later 续播 seek。
// 播放键第一次按下时才 resumeLastPath() → playlist-play-index → 载入并
// 由 mpv 恢复上次进度（与列表正常启动同一条路径）。
void PlayerViewModel::restorePlaylistIdle() {
    if (!mpv_) return;
    int cur = -1;
    const QStringList files = restoredFiles(&cur);
    if (files.isEmpty()) return;
    for (const QString &f : files)
        mpv_->commandV({"loadfile", f.toUtf8().constData(), "append", nullptr});
    // lastPath_ = 播放键要载入的条目（play() 的 !fileLoaded_ 分支据此走
    // resumeLastPath；path 观察器在未载入时不会覆盖它）。
    if (cur >= 0 && cur < files.size()) {
        lastPath_ = files[cur];
        idlePlayIndex_ = cur;
    }
}

// 恢复列表的持久化来源 + 合法化当前索引：restoredCurrentFile()（启动暂停
// 判定）与 restorePlaylist()（实际装载）共用同一规则，避免两处分叉。
QStringList PlayerViewModel::restoredFiles(int *cur) const {
    const QStringList files = settings_.value("playlist").toStringList();
    int c = settings_.value("playlistCurrent", -1).toInt();
    if (!files.isEmpty() && (c < 0 || c >= files.size()))
        c = 0;
    if (cur) *cur = c;
    return files;
}

QString PlayerViewModel::restoredCurrentFile() const {
    int cur = 0;
    const QStringList files = restoredFiles(&cur);
    return files.isEmpty() ? QString() : files[cur];
}

void PlayerViewModel::noteLiveSeekFailure()
{
    QMetaObject::invokeMethod(this, [this] { markLiveSource("cannot seek"); },
                              Qt::QueuedConnection);
}

bool PlayerViewModel::isLiveSource() const
{
    if (liveSourcePending_)
        return true;
    return !liveSourcePath_.isEmpty() && liveSourcePath_ == lastPath_;
}

void PlayerViewModel::markLiveSource(const char *why)
{
    if (isLiveSource())
        return;
    if (lastPath_.isEmpty())
        liveSourcePending_ = true;   // 检测早于 FILE_LOADED：记 pending，加载完成时归属
    else
        liveSourcePath_ = lastPath_;
    MLOG_INFO("live/linear source (%s) — no resume: playing live edge, dropping saved position", why);
    if (mpv_) {
        mpv_->commandAsync({"delete-watch-later-config", nullptr});   // async：主线程不阻塞
        // mpv 的 quit 命令自身会按 save-position-on-quit 写位置——对直播流必须关掉，
        // 否则我们刚删掉的陈旧 start 会在退出时被重新写回（实测）。
        mpv_->setPropertyFlagAsync("save-position-on-quit", false);
    }
    emit liveSourceDetected();
}

bool PlayerViewModel::hasResumeState(const QString &path) {
    if (path.isEmpty()) return false;
    const std::string dir = MpvController::watchLaterDir();
    if (dir.empty()) return false;
    // 条目名 = MD5(路径字符串) 大写十六进制——mpv 用 libavutil hash 逐字节
    // 摘要 `path`（misc/hash.c 按 "%02X" 输出），不做路径规范化。
    const QByteArray digest = QCryptographicHash::hash(
        path.toUtf8(), QCryptographicHash::Md5).toHex().toUpper();
    const QString entry = QString::fromStdString(dir) + QLatin1Char('/')
                        + QString::fromLatin1(digest);
    QFile f(entry);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    while (!f.atEnd()) {
        if (f.readLine().startsWith("start="))
            return true;   // 只有带位置才算"有历史进度"
    }
    return false;
}

// ── Playback（乐观更新：本地状态立即反映，mpv 回写校正）──────────────

void PlayerViewModel::play() {
    if (!mpv_) return;
    if (!fileLoaded_) {
        // 没有任何重播目标（列表为空）→ 保持 idle，不要凭空载入/追加
        if (lastPath_.isEmpty() && idlePlayIndex_ < 0) return;
        resumeLastPath();
    }
    playing_ = true;
    emit playingChanged();
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyFlagAsync("pause", false);
}

// idle 重播（stop 或正常播完后的再次播放）：
// stop keep-playlist 保留列表 → 先查 PlaylistModel 定位 lastPath 原
// 条目，playlist-play-index 直接播放（列表不变、不重复入列）。
// 不用 loadfile replace——cmd_loadfile 的 REPLACE 会先 playlist_clear
// 清空整个列表（曾致 stop 后重启播放列表只剩一条）。
// 列表已无该路径（外部改动/单文件）→ append-play 追加并播放。
void PlayerViewModel::resumeLastPath() {
    if (lastPath_.isEmpty()) return;
    int idx = playlistModel_.indexOfPath(lastPath_);
    if (idx < 0) {
        // 启动 idle：播放列表在 mpv 里已就绪，但 playlistModel_ 由观察器
        // 异步回填——尚未回填时用记录的原始索引兜底（消费后清零，避免
        // 列表后来变化时误用陈旧索引）。
        idx = idlePlayIndex_;
        idlePlayIndex_ = -1;
    }
    if (idx >= 0)
        mpv_->commandV({"playlist-play-index",
                        std::to_string(idx).c_str(), nullptr});
    else
        mpv_->commandV({"loadfile", lastPath_.toUtf8().constData(),
                        "append-play", nullptr});
}

void PlayerViewModel::pause() {
    if (!mpv_) return;
    playing_ = false;
    emit playingChanged();
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyFlagAsync("pause", true);
}

void PlayerViewModel::setPaused(bool p) {
    if (!mpv_) return;
    if (!p) {
        play();   // idle 时重载 lastPath（与 RPC set_property pause no 语义一致）
        return;
    }
    playing_ = false;
    emit playingChanged();
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyFlagAsync("pause", true);
}

// 无 mute 状态位：m=静音（记音量+置 0）/ 取消=恢复。与 toggleMute
// 等价（幂等：状态相同则无操作，RPC 重复 set 安全）。
void PlayerViewModel::setMuted(bool m) {
    if (!mpv_) return;
    if (m == muted()) return;
    toggleMute();
}

void PlayerViewModel::frameStep(int dir) {
    if (!mpv_) return;
    // frame-step 在暂停态推进 1 帧后保持暂停；frame-back-step 回退 1 帧
    // （mpv 0.38+，内部 seek 到前一帧）。参数必须分解传递（commandAsync
    // 不做整行解析）。
    if (dir > 0)
        mpv_->commandAsync({"frame-step", nullptr});
    else
        mpv_->commandAsync({"frame-back-step", nullptr});
}

void PlayerViewModel::togglePlayPause() {
    if (!mpv_) return;
    if (!fileLoaded_) {
        // 停止/正常播完（无活动文件）：重播 lastPath（resumeLastPath
        // 保留列表；loadfile replace 会清空整个列表，不可用）。
        if (lastPath_.isEmpty()) return;
        resumeLastPath();
        // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
        mpv_->setPropertyFlagAsync("pause", false);
        playing_ = true;
        emit playingChanged();
        return;
    }
    bool p = !playing_;
    playing_ = p;
    emit playingChanged();
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyFlagAsync("pause", !p);
}

void PlayerViewModel::stop() {
    if (!mpv_) return;
    playing_ = false;
    emit playingChanged();
    // 乐观复位 idle 状态——path 观察（事件线程→主线程队列）落地前
    // 快速点击播放不会误走"取消暂停"分支（mpv 已 idle，无效）。
    fileLoaded_ = false;
    // keep-playlist：mpv stop 默认清空播放列表（command.c cmd_stop
    // flags&1 → playlist_clear 跳过），单曲循环下停止后列表消失。
    // async：GUI 线程不阻塞 core-lock（同 seek；同步命令会与软解 DR 帧
    // 分配互锁，见 path 观察器注释）。
    mpv_->commandAsync({"stop", "keep-playlist", nullptr});
}

void PlayerViewModel::seekAbsolute(int64_t ms) {
    if (!mpv_) return;
    // 标记 seek 在途（门控见 seekRelative）：完成信号 = MPV_EVENT_PLAYBACK_RESTART
    // （notePlaybackRestart 清除）；seekIssueMs_ 只用于失败兜底。
    seekInFlight_.store(true);
    seekIssueMs_.store(steadyNowMs());
    currentTime_.store(ms);
    emit currentTimeChanged();
    // %.3f 保留亚秒精度（ms/1000 整数除会丢 0.5s → "seek 0 absolute" 从头重启）。
    // 参数必须分解传递：commandAsync 不做整行解析（整串会被当 target 解析失败）。
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", ms / 1000.0);
    // 先告知 vf_vsr 目标：目标之前的帧（关键帧→目标）在滤镜里直接丢弃。
    // mpv 的 hr-seek framedrop 只在解码器层按 packet 丢，某些流上不生效 →
    // 这些注定被 VO 丢弃的帧会逐帧跑 VSR（实测一次 seek 白跑 ~28 帧 × 6 ms）。
    // 命令队列有序：标记先于 seek 生效。滤镜侧只在 3 s 过期或下一次
    // seek-target（即下一步）时清除——**不能在首个 >= 目标的帧上清**：mpv
    // 会在链 reset 之后把目标之前的帧再喂一遍（实测 60–130 帧），早清会让
    // 这批帧重新进引擎（2.8 ms/帧 → 单次 seek 多 200–400 ms）。
    if (!isLiveSource())   // 直播流 seek 本来就会失败，标记只会在原地丢帧
        mpv_->commandAsync({"vf-command", "vsr", "seek-target", buf, nullptr});
    mpv_->commandAsync({"seek", buf, "absolute", nullptr});   // async：主线程不阻塞 core-lock
}

void PlayerViewModel::seekRelative(int64_t offsetMs) {
    if (!mpv_) return;
    // 连续快进/快退门控：上一次 seek 还没明确成功（mpv 尚未 restart complete，
    // 即帧还没显示出来）时，本次步进直接丢弃——不排队、不累加。按住方向键
    // 时每次只在前一步真正出帧后才推进，避免 mpv 内部合并成整段跳画面。
    // 兜底：不可 seek 的流上 mpv 不会发 seeking，超过 kSeekGateTimeoutMs 放行，
    // 避免方向键被永久锁死。直播流（isLiveSource）本来每次 seek 都会被 mpv
    // 拒绝，不参与门控——否则按下后 5 s 内的按键全被丢掉。
    if (!isLiveSource() && seekInFlight_.load()) {
        if (steadyNowMs() - seekIssueMs_.load() < kSeekGateTimeoutMs)
            return;
        seekInFlight_.store(false);
    }
    int64_t target = currentTime_ + offsetMs;
    if (target < 0) target = 0;
    if (duration_ > 0 && target > duration_) target = duration_;
    seekAbsolute(target);
}

void PlayerViewModel::notePlaybackRestart() {
    // 事件线程：MPV_EVENT_PLAYBACK_RESTART = 一次 seek（或加载）完成、目标帧
    // 已显示。这是门控的完成信号（属性观察会被 libmpv 值去重，连续 seek 时
    // 最后一条 true/false 可能不送达，见 header 注释）。
    bool seeking = mpv_ && mpv_->propertyFlag("seeking");
    if (!seekInFlight_.load()) return;
    // 迟到的旧 seek 完成事件：新 seek 已经开始（seeking 仍为真）时忽略，
    // 否则会把新 seek 误判成已完成、提前放行下一步。
    if (seeking) return;
    seekInFlight_.store(false);
}

// ── Audio（乐观更新）────────────────────────────────────────────────

void PlayerViewModel::setVolume(double vol) {
    if (!mpv_) return;
    if (vol < 0.0) vol = 0.0;
    if (vol > 1.0) vol = 1.0;
    // 乐观更新：QML 立即反馈，mpv volume 观察事件随后回写校正。
    // 静音为派生状态（volume==0），无需额外状态迁移。
    if (volume_ != vol) { volume_ = vol; emit volumeChanged(); }
    saveSettings("volume", vol);
    // 转发 + 节流：slider 拖动高频调用只记录最新值，事件循环每迭代
    // 合并投递一次（async，主线程零阻塞——避免同步 set 与 flip_page
    // 的 core-lock 互锁窗口，见 PlayerViewModel 线程模型注释）。
    pendingVolume_ = vol;
    if (!volumeSetQueued_) {
        volumeSetQueued_ = true;
        QMetaObject::invokeMethod(this, [this] {
            volumeSetQueued_ = false;
            mpv_->setPropertyDoubleAsync("volume", pendingVolume_ * 100.0);
        }, Qt::QueuedConnection);
    }
}

// 静音 = 音量 0：进入静音记录当前音量（过小则记可听下限），退出恢复。
// 全程只操作 volume 属性（mpv `mute` 属性不使用），乐观更新走 setVolume。
void PlayerViewModel::toggleMute() {
    if (!mpv_) return;
    if (muted()) {
        setVolume(savedVolume_);
    } else {
        // 音量过小（如 slider 拖到 0 附近）→ 记录可听下限，退出静音后可听
        savedVolume_ = std::max(volume_, kMinUnmuteVolume);
        setVolume(0.0);
    }
}

void PlayerViewModel::toggleHwaccel() {
    if (!mpv_) return;
    // 当前文件只能软解时按钮在 UI 里是禁用状态（hwDecodeAvailable=false），
    // 这里再兜一层：不因为一次点击把偏好改掉。
    if (!hwDecodeAvailable_.load()) return;
    if (hwdecInit_.empty())
        hwdecInit_ = mpv_->propertyString("hwdec");
    bool target = !hwDecodeEnabled_;
    // ⚠ 只翻**偏好**，不改 `hwDecoding_`（实际状态）：实际值只由
    // hwdec-current 观察器回写。软解回退（本文件不支持硬解）因此不会把
    // 用户的"硬解"设置冲成软解——换到支持硬解的文件时自动恢复硬解。
    hwDecodeEnabled_.store(target);
    emit hwDecodeEnabledChanged();
    // **只改选项，不重载文件**：mpv 原生支持运行期切 hwdec——该选项
    // flags = UPDATE_HWDEC，player/command.c 的处理是
    // `mp_decoder_wrapper_control(dec, VDCTRL_REINIT)`（重建解码器，
    // 因此不会有"新解码器缺参考帧 → Could not find ref with POC"）
    // + `queue_seek(last_pts, MPSEEK_EXACT)`（自己精确 seek 回原位置）。
    // ⚠ 2026-10-22 更正：旧的 `loadfile <path> replace` 实现会
    //   playlist_clear（播放列表只剩当前文件——用户报"切软解/硬解会清空
    //   播放列表"），还会丢掉外部字幕/轨道记忆；mpv 本来就不需要它。
    // async：主线程同步写会等 dispatch 队列清空，与 decode DR 分配任务互等死锁。
    mpv_->setPropertyStringAsync("hwdec", target ? hwdecInit_ : "no");
}

// ── Video settings ───────────────────────────────────────────────────

void PlayerViewModel::setScale(double s) {
    if (!mpv_) return;
    if (!aiAllowed("scale")) return;
    if (s < -1 || s > 4 || s == 1) return;
    // 请求倍率直接生效，无输入分辨率保护——引擎实际硬限制 = 输出 ≤8K
    //（实测 1080p 4×→7680×4320 成功），超出引擎能力的请求由引擎侧处理。
    bool wasActive = vsrActive();
    if (fabs(scale_ - s) > 0.001) { scale_ = s; emit scaleChanged(); }
    saveSettings("scale", s);
    pushVf("vsr", "scale", scaleStr());  // 热更新，不重建 filter 链
    bool nowActive = vsrActive();
    if (wasActive != nowActive) emit vsrActiveChanged();
}

void PlayerViewModel::setQuality(int q) {
    if (!mpv_) return;
    if (!aiAllowed("quality")) return;
    if (q < 1 || q > 4) return;
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
    saveSettings("quality", q);
    pushVf("vsr", "quality", qualityStr());
}

void PlayerViewModel::setDenoiseQuality(int d) {
    if (!mpv_) return;
    if (!aiAllowed("denoise")) return;
    if (d != -1 && (d < 8 || d > 11)) return;
    bool wasActive = vsrActive();
    if (denoise_ != d) { denoise_ = d; emit denoiseQualityChanged(); }
    saveSettings("denoiseQuality", d);
    pushVf("vsr", "denoise", denoiseStr());
    bool nowActive = vsrActive();
    if (wasActive != nowActive) emit vsrActiveChanged();
}

void PlayerViewModel::setFrucFps(int v) {
    if (!mpv_) return;
    if (!aiAllowed("fruc")) return;
    if (!fruc::isValidPreset(v)) return;
    if (frucFps_ != v) { frucFps_ = v; emit frucFpsChanged(); }
    saveSettings("frucFps", v);
    pushVf(frucFilterName(), "fps", v == -1 ? "off" : std::to_string(v));
}

// UI 档位模型：label 走 tr()（QML 的 qsTr 迁到 C++，context 由 QualityPopup
// 变为 PlayerViewModel——.ts 已补 "Off" 条目）。value 语义 = frucFps_。
QVariantList PlayerViewModel::frucModel() const {
    QVariantList out;
    for (std::size_t i = 0; i < fruc::kPresetCount; ++i) {
        QVariantMap m;
        m["label"] = tr(fruc::kPresets[i].label);
        m["value"] = fruc::kPresets[i].value;
        out.append(m);
    }
    return out;
}

// 质量档热切换（仅 VFG）：vf-command `vfg mode <low|medium|high>`——filter
// 侧下一帧检测 Mode 不一致 → 重建引擎（不重建 filter 链）。RIFE 分支无此
// 档：不生效（CLI/RPC 路径各有一致处理）。
void PlayerViewModel::setFrucQuality(int v) {
    if (!mpv_) return;
    if (!fruc::kModeSelectable) return;
    if (!fruc::isValidMode(v)) return;
    if (frucQuality_ != v) { frucQuality_ = v; emit frucQualityChanged(); }
    saveSettings("frucQuality", v);
    pushVf(frucFilterName(), "mode", fruc::modeName(v));
}

// 质量档 UI 模型（同 frucModel：label 走 tr()，context = PlayerViewModel；
// RIFE 分支该排隐藏，模型照常产出但不消费）。
QVariantList PlayerViewModel::frucQualityModel() const {
    QVariantList out;
    for (std::size_t i = 0; i < fruc::kModePresetCount; ++i) {
        QVariantMap m;
        m["label"] = tr(fruc::kModePresets[i].label);
        m["value"] = fruc::kModePresets[i].value;
        out.append(m);
    }
    return out;
}

bool PlayerViewModel::frucQualityAvailable() const { return fruc::kModeSelectable; }

// 输出帧率上限热切换（仅 VFG）：vf-command `vfg max-fps <n>`——filter 下一帧
// 直通判定重估（超过上限 → 直通，reason=max-fps；没超过就照常插帧）。
void PlayerViewModel::setFrucMaxFps(int v) {
    if (!mpv_) return;
    if (!fruc::kMaxFpsSelectable) return;
    if (!fruc::isValidMaxFps(v)) return;
    if (frucMaxFps_ != v) { frucMaxFps_ = v; emit frucMaxFpsChanged(); }
    saveSettings("frucMaxFps", v);
    pushVf(frucFilterName(), "max-fps", std::to_string(v));
}

// 上限 UI 模型（同 frucQualityModel：label 走 tr()，context = PlayerViewModel；
// RIFE 分支该排隐藏，模型照常产出但不消费）。
QVariantList PlayerViewModel::frucMaxFpsModel() const {
    QVariantList out;
    for (std::size_t i = 0; i < fruc::kMaxFpsPresetCount; ++i) {
        QVariantMap m;
        m["label"] = tr(fruc::kMaxFpsPresets[i].label);
        m["value"] = fruc::kMaxFpsPresets[i].value;
        out.append(m);
    }
    return out;
}

bool PlayerViewModel::frucMaxFpsAvailable() const { return fruc::kMaxFpsSelectable; }

// ── 反交错（mpv `--deinterlace` 的三态开关）────────────────────────────
// 全部实现由 mpv 输出链的 `deint` 前置节点承担（filters/f_auto_filters.c：
// 按帧的 interlaced 标志决定、按像素格式选 bwdif_cuda/bwdif_vulkan/bwdif），
// 位置在 user_filters（`--vf`，即我们的 hwup→vsr→truehdr→vfg）**之前**。
// 这里只做三件事：把模式映射成 mpv 值、下发、持久化。

int PlayerViewModel::parseDeintMode(const std::string &s) {
    if (s == "off" || s == "no")
        return 0;
    if (s == "auto")
        return 1;
    if (s == "force" || s == "yes")
        return 2;
    return -1;
}

namespace {
const char *deintMpvValue(int mode) {
    return mode == 1 ? "auto" : (mode == 2 ? "yes" : "no");
}
}  // namespace

void PlayerViewModel::initDeinterlace(int cliMode) {
    // CLI 显式指定 → 本会话以它为准（不写回 QSettings：命令行不该改持久化）；
    // 否则用持久化值（loadSettings 已夹到 0..2）。
    deintMode_.store(cliMode >= 0 ? cliMode : persistDeintMode_);
}

void PlayerViewModel::applyDeinterlace() {
    if (!mpv_) return;
    // `options/<name>` 是 mpv 暴露选项的通用属性路径（DOCS/man/input.rst
    // "options/<name> (RW)"）；deint 节点每帧 m_config_cache_update() 读它，
    // 所以切换即时生效、不重建用户滤镜链（VSR/VFG 引擎保留）。
    mpv_->commandAsync({"set", "options/deinterlace",
                        deintMpvValue(deintMode_.load()), nullptr});
}

void PlayerViewModel::setDeintMode(int mode) {
    if (mode < 0 || mode > 2) return;
    if (deintMode_.load() != mode) {
        deintMode_.store(mode);
        emit deintModeChanged();
    }
    applyDeinterlace();
    saveSettings("deintMode", mode);
}

void PlayerViewModel::setRotMode(int deg) {
    // 只接受 0/90/270（菜单三态；180 用不到——横竖互换只需要 90/270）。
    if (deg != 0 && deg != 90 && deg != 270)
        return;
    if (rotMode_.load() != deg) {
        rotMode_.store(deg);
        emit rotModeChanged();
    }
    // 链上恒有 @rot（angle=init）→ 运行期只改 filter 实例选项，不重建链；
    // 下一帧即按新角度转（光栅在显存里做，无下载/上传）。
    if (mpv_)
        mpv_->commandAsync({"vf-command", "rot", "angle",
                            std::to_string(deg).c_str(), nullptr});
    saveSettings("rotMode", deg);
}

// ── AI 可用性（Vulkan 设备 × CUDA 设备同一性）──────────────────────────
// 超分/插帧/上变换都是 CUDA 产出 + Vulkan 零拷贝导入，**必须同一张卡**。
// 启动时用 UUID 判一次（CudaProbe），判不出来就整条链直通——多卡机器
// （笔记本 iGPU+dGPU、CUDA_VISIBLE_DEVICES 只暴露一张）宁可降级，不要黑屏。

void PlayerViewModel::initAiAvailability(bool available, const std::string &reason,
                                        const std::string &uuidHex) {
    aiUnavailableReason_ = available ? std::string() : reason;
    aiCudaUuid_ = uuidHex;
    if (aiAvailable_.load() != available) {
        aiAvailable_.store(available);
        emit aiAvailableChanged();
    }
    if (!available) {
        MLOG_WARN("AI processing unavailable: %s", reason.c_str());
        MLOG_WARN("  -> upscaling/interpolation/SDR->HDR10 are forced to passthrough for this run "
                  "(persisted settings are untouched). To use them, make Vulkan and CUDA land on "
                  "the same GPU (laptops usually need dGPU-only mode / PRIME render offload, "
                  "or drop CUDA_VISIBLE_DEVICES)");
    } else {
        MLOG_INFO("AI processing available: %s", reason.c_str());
    }
}

bool PlayerViewModel::aiAllowed(const char *what) {
    if (aiAvailable_.load()) return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        MLOG_WARN("AI unavailable (%s): ignoring subsequent %s etc. settings",
                  aiUnavailableReason_.c_str(), what);
    }
    return false;
}

// 剥离 mpv 日志消息的尾随换行与前导空格（mpv 的 log text 以 \n 结尾——
// 原样存入会让 OSD 行内嵌 \n → 行距异常/提前折行；"vsr-status:" 前缀后
// 紧跟空格——提取处 text+N 保留前导空格，拼入 OSD 状态段会与前面
// 元素（渲染分辨率）产生多余间距，见 osdTextString）
static void stripEol(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    size_t b = s.find_first_not_of(' ');
    if (b != std::string::npos && b > 0)
        s.erase(0, b);
}

void PlayerViewModel::setFrucStatus(const std::string &s) {
    std::lock_guard<std::mutex> lk(frucStatusMtx_);
    frucStatus_ = s;
    stripEol(frucStatus_);
}

void PlayerViewModel::setVsrStatus(const std::string &s) {
    std::lock_guard<std::mutex> lk(vsrStatusMtx_);
    vsrStatus_ = s;
    stripEol(vsrStatus_);
}

void PlayerViewModel::setHdrStatus(const std::string &s) {
    std::lock_guard<std::mutex> lk(hdrStatusMtx_);
    hdrStatus_ = s;
    stripEol(hdrStatus_);
}

void PlayerViewModel::setHdrOutput(const std::string &s) {
    std::lock_guard<std::mutex> lk(hdrOutputMtx_);
    hdrOutput_ = s;
    stripEol(hdrOutput_);
}

// ── Speed / window / OSD ─────────────────────────────────────────────

void PlayerViewModel::setAspect(const QString &v) {
    if (!mpv_) return;
    if (aspect_ != v) { aspect_ = v; emit aspectChanged(); }
    saveSettings("aspect", v);
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyStringAsync("video-aspect-override", v.toStdString());
}

void PlayerViewModel::setSpeed(double speed) {
    if (!mpv_) return;
    if (speed < 0.1) speed = 0.1;
    if (speed > 4.0) speed = 4.0;
    if (speed_.load() != speed) { speed_.store(speed); emit speedChanged(); }
    saveSettings("speed", speed);
    // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
    mpv_->setPropertyDoubleAsync("speed", speed);
}

void PlayerViewModel::setFullscreen(bool fs) {
    if (fullscreen_ != fs) {
        fullscreen_ = fs;
        emit fullscreenChanged();
    }
}

void PlayerViewModel::toggleFullscreen() {
    setFullscreen(!fullscreen_);
}

void PlayerViewModel::setOverlaysVisible(bool v) {
    if (overlaysVisible_ != v) {
        overlaysVisible_ = v;
        emit overlaysVisibleChanged();
    }
}

void PlayerViewModel::showOsd() {
    if (osdVisible_.load()) return;
    osdVisible_.store(true);
    emit osdVisibleChanged();
    fpsTimer_.restart();       // 基准：Δt 从此刻起算
    renderFps_.store(0.0);     // 首拉取显示 0.0（不显示陈旧值）
    osdLevel_.store(true);
}

void PlayerViewModel::toggleOsd() {
    osdVisible_.store(!osdVisible_.load());
    emit osdVisibleChanged();
    if (osdVisible_.load()) {
        // 打开：osd-level 由事件线程 idle 回调应用（≤100ms）；
        // 渲染帧率差值基准重置（osdTextString 首拉取时从 0 起算）
        fpsTimer_.restart();       // 基准：Δt 从此刻起算
        renderFps_.store(0.0);     // 重开后首秒显示 0.0（不显示陈旧值）
        osdLevel_.store(true);
    } else {
        osdLevel_.store(false);
    }
}

// ── Loop / playlist ──────────────────────────────────────────────────

void PlayerViewModel::setLoopMode(int m) {
    if (!mpv_ || m < 0 || m > 2) return;
    switch (m) {
    case 1:
        // async（主线程 UI 操作——同步 set 死锁风险同 FILE_LOADED 根因）
        mpv_->setPropertyStringAsync("loop-file", "inf");
        mpv_->setPropertyStringAsync("loop-playlist", "no");
        break;
    case 2:
        mpv_->setPropertyStringAsync("loop-file", "no");
        mpv_->setPropertyStringAsync("loop-playlist", "inf");
        break;
    default:
        mpv_->setPropertyStringAsync("loop-file", "no");
        mpv_->setPropertyStringAsync("loop-playlist", "no");
        break;
    }
    if (loopMode_ != m) { loopMode_ = m; emit loopModeChanged(); }
    saveSettings("loopMode", m);
}

void PlayerViewModel::toggleLoop() {
    if (!mpv_) return;
    setLoopMode((loopMode_ + 1) % 3);
}

// 播放中的控制命令一律 async：GUI 线程同步调 mpv API 会在软解 + vo=libmpv
// 时与 DR 帧分配互锁（解码线程等 GUI 服务 DR，GUI 等 core 执行命令；
// 见 path 观察器处的完整说明）。启动/idle 阶段的 loadfile（restorePlaylist*
// /resumeLastPath）保持同步——那时 core 空闲，没有解码线程在等 DR。
void PlayerViewModel::playlistNext() { if (mpv_) mpv_->commandAsync({"playlist-next", nullptr}); }
void PlayerViewModel::playlistPrev() { if (mpv_) mpv_->commandAsync({"playlist-prev", nullptr}); }

void PlayerViewModel::playlistPlayIndex(int index) {
    if (!mpv_) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", index);
    mpv_->commandAsync({"playlist-play-index", buf, nullptr});
}

// ── File ─────────────────────────────────────────────────────────────

void PlayerViewModel::loadFile(const QString &path) {
    if (!mpv_) return;
    videoInfo_ = path.section('/', -1);
    emit videoInfoChanged();
    mpv_->commandAsync({"loadfile", path.toUtf8().constData(), nullptr});
}

// ── 轨道/字幕/文件/列表管理（Task 3 计划代码；moc 要求 slot 有定义）──

void PlayerViewModel::openFiles(const QStringList &pathsIn, int mode) {
    if (!mpv_ || pathsIn.isEmpty()) return;
    QStringList paths;
    for (const QString &p : pathsIn) paths.append(toLocalPath(p));
    if (mode == 2) {   // 加载字幕
        for (const QString &p : paths) {
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
            noteExternalSubAdded(p);   // 即时落盘（pending 暂存未回写路径）
        }
        return;
    }
    if (mode == 4) {   // 打开 URL：网络流播放（toLocalPath 原样透传），
        // 跳过混拖分类（URL 以 .srt 结尾会被误判为字幕 sub-add）
        bool first = true;
        for (const QString &p : paths) {
            if (first) mpv_->commandAsync({"loadfile", p.toUtf8().constData(), nullptr});
            else       mpv_->commandAsync({"loadfile", p.toUtf8().constData(), "append", nullptr});
            first = false;
        }
        return;
    }
    // 文件夹不在此扫描：目录路径原样交给 mpv 的 loadfile，由 mpv 的
    // demux_playlist 展开（--directory-mode 默认 lazy）——顶层媒体按 mpv
    // 的扩展名表（video/audio/image/archive/playlist）展开，子目录作为
    // 条目留在列表里、点击时才再展开。与拖放（mode 0/1）同一条通路，
    // 所以"打开文件夹"与拖入文件夹的行为一致。
    // 混拖分类：字幕文件 → sub-add，其余 → loadfile（混合目录场景）
    bool first = true;
    for (const QString &p : paths) {
        if (isSubtitleFile(p)) {
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
            noteExternalSubAdded(p);   // 即时落盘（pending 暂存未回写路径）
            continue;
        }
        if (mode == 1 || !first)
            mpv_->commandAsync({"loadfile", p.toUtf8().constData(), "append", nullptr});
        else
            mpv_->commandAsync({"loadfile", p.toUtf8().constData(), nullptr});
        first = false;
    }
}

void PlayerViewModel::selectTrack(const QString &type, qlonglong id) {
    if (!mpv_) return;
    if (type == "audio")      { mpv_->setPropertyInt64Async("aid", id); }
    else if (type == "video") { mpv_->setPropertyInt64Async("vid", id); }
    else if (type == "sub") {
        if (id >= 0) {
            mpv_->setPropertyInt64Async("sid", id);
            // 手动选字幕轨 = 要求显示（"清除字幕设置"已关可见性）
            if (!subVisible_) { subVisible_ = true; emit subVisibleChanged(); }
            mpv_->setPropertyFlagAsync("sub-visibility", true);
        } else {
            mpv_->setPropertyStringAsync("sid", "no");
        }
    }
    // 乐观更新 selected 标志（sub<0 = 关闭 → 全部取消）：
    // 即时落盘要求保存反映刚发生的选择——mpv 异步回写 trackList_
    // 前本地已是新状态；同时 UI 立即高亮。命令失败由观察器回写自愈。
    for (QVariant &t : trackList_) {
        QVariantMap m = t.toMap();
        if (m["type"] == type) m["selected"] = (m["id"].toLongLong() == id);
        t = m;
    }
    emit trackListChanged();
    saveTrackMemory();   // 即时落盘（按文件）
}

void PlayerViewModel::toggleSubtitles() {
    if (!mpv_) return;
    // 乐观翻转（观察器回填校正——双击连击基于本地新值计算，
    // 避免陈旧状态发送相同目标值）
    subVisible_ = !subVisible_;
    emit subVisibleChanged();
    mpv_->setPropertyFlagAsync("sub-visibility", subVisible_);
    saveTrackMemory();   // 即时落盘（按文件；subVisible_ 已乐观更新）
}

void PlayerViewModel::adjustSubDelay(double delta) {
    if (!mpv_) return;
    // 乐观累加（绝对赋值：连续两次调节后终值正确）
    subDelay_ += delta;
    emit subDelayChanged();
    mpv_->setPropertyDoubleAsync("sub-delay", subDelay_);
    saveTrackMemory();   // 即时落盘（按文件；subDelay_ 已乐观更新）
}

void PlayerViewModel::resetSubDelay() {
    if (!mpv_) return;
    if (subDelay_ != 0.0) { subDelay_ = 0.0; emit subDelayChanged(); }
    mpv_->setPropertyDoubleAsync("sub-delay", 0.0);
    saveTrackMemory();   // 即时落盘（按文件；subDelay_ 已乐观更新）
}

void PlayerViewModel::playlistRemove(int index) {
    if (!mpv_) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", index);
    mpv_->commandAsync({"playlist-remove", buf, nullptr});
}

// ── 当前条目：从列表移除 / 删除文件 ─────────────────────────────────
// 前进语义完全交给 mpv 的 `playlist-remove`（2026-10-23 实测）：
//  · 移除**当前**条目 → playlist_remove 把 current 指向后一条 + PT_NEXT_ENTRY，
//    mpv 自己载入下一条；
//  · `pause` 是全局属性，跨曲目保持 → 暂停状态下新条目载入但不播放
//    （time-pos 不动），符合"当前暂停时下一个不立即播放"；
//  · 没有后继且 loop-playlist=no → 列表清空（count=0 / pos=-1 /
//    idle-active=yes），即"停止并保持 idle"；
//  · loop-playlist=inf 且移除最后一个 → mpv 回到剩余第一条继续（不 idle）。
// 所以这里不需要自己判断"有没有下一条/要不要停"。

void PlayerViewModel::removeCurrentFromPlaylist() {
    if (!mpv_) return;
    // 用缓存的 playlist-pos（-1 = 无当前条目）；**不在 GUI 线程同步读**
    //（同步 mpv API 与软解 DR 帧分配互锁，见 path 观察器注释）。也不用
    // lastPath_（曲目切换期间 path 属性会被清空）。
    const int pos = playlistPos_;
    if (pos < 0) {
        MLOG_INFO("playlist: no current entry to remove");
        return;
    }
    playlistRemove(pos);
    MLOG_INFO("playlist: removed current entry %d", pos);
}

void PlayerViewModel::deleteCurrentFile() {
    // 路径同样用缓存（与 canDeleteCurrentFile_ 同源），不重读 mpv 属性。
    const QString path = deletePath_;
    if (!canDeleteCurrentFile_ || path.isEmpty()) {
        MLOG_WARN("file: delete refused (not a local writable file): %s",
                  path.isEmpty() ? "(none)" : path.toUtf8().constData());
        return;
    }
    // 移到回收站（XDG trash spec；gio 在 KDE/GNOME 都可用）。失败则**不动播放
    // 列表**——文件还在，列表条目不该凭空消失。
    QProcess proc;
    proc.start(QStringLiteral("gio"), {QStringLiteral("trash"), QStringLiteral("--"), path});
    const bool finished = proc.waitForFinished(5000);
    if (!finished || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        MLOG_ERR("file: failed to move to trash: %s (%s)", path.toUtf8().constData(),
                 finished ? proc.readAllStandardError().constData() : "timeout");
        return;
    }
    MLOG_INFO("file: moved to trash: %s", path.toUtf8().constData());
    removeCurrentFromPlaylist();
}

void PlayerViewModel::refreshCanDeleteCurrentFile(const QString &path) {
    // path 由调用方传入（观察器的已读值 / 缓存）——本函数在 GUI 线程跑，
    // 不能同步读 mpv 属性（原因见 path 观察器处的注释）。
    bool can = false;
    // 只允许本地常规可写文件：URL/流（含 http(s)/ytdl）没有"文件"可删；
    // 相对路径（命令行 `input/foo.mkv`）按当前工作目录解析。
    if (!path.isEmpty() && !path.contains(QStringLiteral("://"))) {
        QFileInfo fi(path);
        const QFileInfo abs(fi.isAbsolute() ? path : QDir::current().absoluteFilePath(path));
        can = abs.isFile() && abs.isWritable();
    }
    deletePath_ = path;   // 与 canDeleteCurrentFile_ 同源：删除时用同一路径
    if (canDeleteCurrentFile_ != can) {
        canDeleteCurrentFile_ = can;
        MLOG_INFO("file: delete availability = %s (%s)", can ? "yes" : "no",
                  path.isEmpty() ? "(no media)" : path.toUtf8().constData());
        emit canDeleteCurrentFileChanged();
    }
}

// ── 外部字幕扫描（轨道弹窗"字幕"页签数据源）────────────────────────
// 主线程调用（文件 IO ~ms 级、低频：文件加载时）。目录全部字幕文件
// 常显（不因加载状态移除）；点击时 QML 侧对照 trackList 决定加载
//（sub-add select）或选择现有轨。优先级：
//   0 = 精确同名（video.mp4 ↔ video.srt）
//   1 = 语言后缀（video.zh.srt / video.en-US.ass——basename 前缀匹配）
//   2 = 其余按文件名
void PlayerViewModel::scanSubtitleFiles(const QString &videoPath) {
    QVariantList files;
    if (!videoPath.isEmpty()) {
        QFileInfo vi(videoPath);
        QDir dir(vi.dir());
        const QStringList exts = {"*.srt", "*.ass", "*.ssa", "*.vtt",
                                  "*.sub", "*.sbv"};
        QStringList names = dir.entryList(exts, QDir::Files, QDir::Name);
        // 优先级（单点定义，排序与条目共用；entryList Name 已排序，
        // 同级保持文件名序）：
        //   0 = 精确同名（video.mp4 ↔ video.srt）
        //   1 = 命名后缀（video.zh.srt / "video zh-cn.ass"——点或空格分隔）
        //   2 = 其余按文件名（不匹配视频名——不自动加载）
        const QString base = vi.completeBaseName();
        auto prio = [&base](const QString &n) {
            const QString stem = n.section('.', 0, -2);   // 去扩展名
            if (stem == base) return 0;
            if (stem.startsWith(base + ".") || stem.startsWith(base + " ")) return 1;
            return 2;
        };
        std::stable_sort(names.begin(), names.end(),
            [&prio](const QString &a, const QString &b) {
                int pa = prio(a), pb = prio(b);
                if (pa != pb) return pa < pb;
                return a < b;
            });
        for (const QString &n : names)
            files.append(QVariantMap{{"name", n}, {"path", dir.filePath(n)},
                                     {"prio", prio(n)}});
    }
    if (subtitleFiles_ != files) { subtitleFiles_ = files; emit subtitleFilesChanged(); }
}

// ── 轨道记忆（按文件，QSettings trackMem map + trackMemOrder LRU）──
// 即时落盘：每个轨道设置动作（selectTrack/toggleSubtitles/
// adjustSubDelay/sub-add）完成后同步写盘——保存点在动作处乐观更新
//（selected 标志翻转 / pendingSubs_ 暂存），记忆永远反映最近一次
// 用户操作，无需等待 mpv 异步回写。内容 = 弹框全部设置项：
// aid/sid/vid 选中 + 外部字幕（subs/sel）+ subVisible/subDelay。
// 容量上限 10 个文件（trackMemOrder 头部 = 最近使用，超限淘汰尾部；
// 恢复即使用，同样前移）。不参与播放器全局设置的开机加载/关闭落盘
// 生命周期（loadSettings/applyPlaybackSettings/main.cpp 退出路径）。
// 恢复：文件加载完成 hook（FILE_LOADED——打开文件与播放列表选取同
// 路径），仅当该路径有记忆时生效（再次打开对应文件才恢复；含 CLI
// 显式文件）。外部字幕恢复 = sub-add（exists 对照 trackList 防重复轨）。
void PlayerViewModel::saveTrackMemory() {
    if (!settingsEnabled_ || lastPath_.isEmpty()) return;
    QVariantMap mem;
    for (const QVariant &t : trackList_) {
        const QVariantMap m = t.toMap();
        const QString type = m["type"].toString();
        const int id = m["id"].toInt();
        if (m["selected"].toBool()) {
            if (type == "audio") mem["aid"] = id;
            else if (type == "video") mem["vid"] = id;
            else if (type == "sub" && !m["external"].toBool()) mem["sid"] = id;
        }
        if (type == "sub" && m["external"].toBool()) {
            const QString f = m["filename"].toString();
            if (!f.isEmpty()) {
                QStringList subs = mem["subs"].toStringList();
                subs.append(f);
                mem["subs"] = subs;
                if (m["selected"].toBool()) mem["sel"] = f;
            }
        }
    }
    // 刚 sub-add 尚未回写 trackList_ 的路径（mpv 异步，毫秒级）——
    // 并入 subs 保证即时落盘完整；trackList_ 回写后自然合并。
    // 最近一次 sub-add select 的路径即为选中（trackList_ 尚未反映）。
    for (const QString &p : pendingSubs_) {
        QStringList subs = mem["subs"].toStringList();
        if (!subs.contains(p)) {
            subs.append(p);
            mem["subs"] = subs;
        }
    }
    if (!mem.contains("sel") && !pendingSubs_.isEmpty())
        mem["sel"] = pendingSubs_.constLast();
    // 字幕偏移（可见性无独立配置：是否显示由 sid 决定，2026-08-06）
    mem["subDelay"] = subDelay_;
    // LRU：trackMem 按 path 键存内容；trackMemOrder 记录访问序
    //（头部 = 最近使用）。超上限（10）淘汰最久未用。
    QVariantMap all = settings_.value("trackMem").toMap();
    QStringList order = settings_.value("trackMemOrder").toStringList();
    all[lastPath_] = mem;
    order.removeAll(lastPath_);
    order.prepend(lastPath_);
    constexpr int kMaxTrackMemFiles = 10;
    while (order.size() > kMaxTrackMemFiles)
        all.remove(order.takeLast());
    // 兜底：order 未覆盖的历史残留键（LRU 引入前的旧数据）一并淘汰，
    // 保证 map 规模与 order 一致（≤10）。
    for (auto it = all.begin(); it != all.end();) {
        if (!order.contains(it.key())) it = all.erase(it);
        else ++it;
    }
    settings_.setValue("trackMem", all);
    settings_.setValue("trackMemOrder", order);
    settings_.sync();   // 即时落盘（QSettings 默认内存缓冲，须显式刷盘）
}

void PlayerViewModel::restoreTrackMemory(const QString &path) {
    if (!mpv_ || path.isEmpty() || !settingsEnabled_) return;
    const QVariantMap all = settings_.value("trackMem").toMap();
    const QVariantMap mem = all.value(path).toMap();
    if (mem.isEmpty()) {   // 无记忆 → 自动字幕策略（locale > 英文 > 目录匹配文件 > 不显示）
        autoSelectSubtitle();
        return;
    }
    // 恢复即使用：LRU 顺序前移（头部 = 最近使用）
    QStringList order = settings_.value("trackMemOrder").toStringList();
    order.removeAll(path);
    order.prepend(path);
    settings_.setValue("trackMemOrder", order);
    settings_.sync();
    // 轨道选择（FILE_LOADED 后 track-list 已填充，设置 aid/sid/vid 生效）。
    // 一律 async（2026-08-10 卡死根因）：本函数在主线程（FILE_LOADED
    // 事件处理）执行——同步 set_property 的 mp_dispatch_lock 会等
    // dispatch 队列清空，而队列里可能有 decode 的 DR 帧分配任务
    //（等 GUI 处理）→ 互等死锁（启动即卡，core dump 实证，见
    // onFileLoadedFromEventThread）。async 投递不阻塞、不拿 core lock。
    if (mem.contains("aid"))
        mpv_->setPropertyStringAsync("aid", std::to_string(mem["aid"].toInt()));
    if (mem.contains("vid"))
        mpv_->setPropertyStringAsync("vid", std::to_string(mem["vid"].toInt()));
    if (mem.contains("sid"))
        mpv_->setPropertyStringAsync("sid", std::to_string(mem["sid"].toInt()));
    // 字幕偏移（可见性无独立记忆——是否显示由 sid 决定）
    if (mem.contains("subDelay"))
        mpv_->setPropertyDoubleAsync("sub-delay", mem["subDelay"].toDouble());
    // 外部字幕（external 轨随文件切换被 mpv 清除，须重新 sub-add）
    const QStringList subs = mem["subs"].toStringList();
    const QString sel = mem["sel"].toString();
    for (const QString &p : subs) {
        if (p.isEmpty()) continue;
        bool exists = false;
        for (const QVariant &t : trackList_) {
            const QVariantMap m = t.toMap();
            if (m["type"] == "sub" && m["external"].toBool() &&
                m["filename"].toString() == p) { exists = true; break; }
        }
        if (exists) continue;
        // mpv_command_async 返回前复制参数（client.c mp_input_parse_cmd_strv）
        if (p == sel)
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
        else
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), nullptr});
    }
}

// ── 字幕自动选择（无记忆加载时，restoreTrackMemory 无记忆分支）──
// 策略（用户定义，2026-08-06）：
//   1. 内置字幕轨按 locale 匹配（系统语言前 2 字母 vs 轨 lang）
//   2. locale 无匹配 → 英文轨
//   3. 无内置匹配 → 目录匹配视频文件名的字幕文件（prio ≤ 1：
//      精确同名 / 点或空格分隔的命名后缀）→ sub-add select
//   4. 其他 → 不显示字幕（sid no，无轨被选）
void PlayerViewModel::autoSelectSubtitle() {
    if (!mpv_) return;
    // 1/2) 内置字幕轨：locale > 英文
    QString sys = QLocale::system().name();
    if (sys.size() >= 2) sys = sys.left(2).toLower();
    int best = -1;
    for (const QVariant &t : trackList_) {
        const QVariantMap m = t.toMap();
        if (m["type"] != "sub" || m["external"].toBool()) continue;
        if (m["lang"].toString().left(2).toLower() == sys) { best = m["id"].toInt(); break; }
    }
    if (best < 0) {
        for (const QVariant &t : trackList_) {
            const QVariantMap m = t.toMap();
            if (m["type"] != "sub" || m["external"].toBool()) continue;
            if (m["lang"].toString().left(2).toLower() == "en") { best = m["id"].toInt(); break; }
        }
    }
    if (best >= 0) { mpv_->setPropertyStringAsync("sid", std::to_string(best)); return; }
    // 3) 目录匹配字幕文件（只取匹配视频名的 prio ≤ 1）
    for (const QVariant &f : subtitleFiles_) {
        const QVariantMap m = f.toMap();
        if (m["prio"].toInt() > 1) continue;
        const QString p = m["path"].toString();
        mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
        noteExternalSubAdded(p);
        return;
    }
    // 4) 其他 → 不显示字幕
    mpv_->setPropertyStringAsync("sid", "no");
}

void PlayerViewModel::noteExternalSubAdded(const QString &path) {
    // sub-add 是异步命令——trackList_ 回写前即时落盘须暂存该路径，
    // 保存时并入 subs；trackList_ 回写后自然合并，不会重复。
    if (!pendingSubs_.contains(path))
        pendingSubs_.append(path);
    saveTrackMemory();
}

void PlayerViewModel::clearSubtitleMemory() {
    if (!settingsEnabled_ || lastPath_.isEmpty()) return;
    QVariantMap all = settings_.value("trackMem").toMap();
    QVariantMap mem = all.value(lastPath_).toMap();
    if (!mem.isEmpty()) {
        // 只清除字幕相关项（音轨/视频轨记忆保留）
        mem.remove("sid");
        mem.remove("subs");
        mem.remove("sel");
        mem.remove("subVisible");
        mem.remove("subDelay");
        if (mem.isEmpty()) {
            // 字幕项全清 → 条目整体删除（trackMemOrder 同步移除，避免
            // 空条目残留在 LRU 序里）
            all.remove(lastPath_);
            QStringList order = settings_.value("trackMemOrder").toStringList();
            order.removeAll(lastPath_);
            settings_.setValue("trackMemOrder", order);
        } else {
            all[lastPath_] = mem;
        }
        settings_.setValue("trackMem", all);
        settings_.sync();
    }
    // ── 运行时清除：当前播放的字幕设置同步重置为默认 ────────────
    // 全部外部字幕轨移除（sub-remove 按轨 id）+ 字幕轨关闭 +
    // 偏移归零 + 可见性恢复默认。异步命令，乐观更新本地状态。
    for (const QVariant &t : trackList_) {
        const QVariantMap m = t.toMap();
        if (m["type"] == "sub" && m["external"].toBool())
            mpv_->commandAsync({"sub-remove",
                                std::to_string(m["id"].toInt()).c_str(), nullptr});
    }
    pendingSubs_.clear();   // 尚未回写 trackList_ 的外部字幕路径一并作废
    mpv_->setPropertyStringAsync("sid", "no");
    // 清除设置即代表不显示字幕（可见性关闭；用户手动选字幕轨时恢复）
    if (subVisible_) { subVisible_ = false; emit subVisibleChanged(); }
    mpv_->setPropertyFlagAsync("sub-visibility", false);
    if (subDelay_ != 0.0) { subDelay_ = 0.0; emit subDelayChanged(); }
    mpv_->setPropertyDoubleAsync("sub-delay", 0.0);
}

void PlayerViewModel::playlistClear() {
    if (!mpv_) return;
    // ⚠ mpv 的 `playlist-clear` **保留当前播放项**（command.c: "Supposed to
    // clear the playlist, except the currently played item"）——那不是"清空"
    // 的语义。先 clear（其余条目删掉、当前项归位 index 0），再移除它 →
    // 真正清空 + 停止（idle）；配合"列表变空 → 忘掉 lastPath_"避免点播放复活。
    mpv_->commandAsync({"playlist-clear", nullptr});
    mpv_->commandAsync({"playlist-remove", "0", nullptr});
}

void PlayerViewModel::screenshot() {
    if (!mpv_) return;
    mpv_->commandAsync({"screenshot", nullptr});
}

// ── vf string ────────────────────────────────────────────────────────

std::string PlayerViewModel::vfOption() const {
    // AI 不可用（Vulkan 设备没有匹配的 CUDA 设备）→ **不插任何滤镜**。
    // 注意连 hwup 也不能留：它会在"另一张卡"上建 CUDA 上下文并输出 CUDA 帧，
    // 交给本卡的 VO 导入只会黑屏（这正是 issue #25 / 笔记本核显场景的形态）。
    if (!aiAvailable_.load())
        return std::string();
    // 链序（编译期后端分支，见 FrucBackend.h）：
    //   VFG：decode → hwup（SW→HW 一致化）→ vsr（超分）→ vfg（插帧）→ VO
    //        为默认（--fruc-order post）；pre 时 vfg 在 vsr 之前处理。
    //   RIFE：decode → hwup → rife（插帧，源分辨率）→ vsr（超分）→ VO，
    //        固定不变（rife 目标语义：2/3/4 = 倍率（benchmark 强制，
    //        决定 decide_mode 的 benchmark 分支）；off/40/48/60 = 目标
    //        帧率（正常模式，按用户设置执行——性能自动降级已移除））。
    // hwup 使插帧/超分只处理 CUDA 帧（软解输入也走 HW 路径）。
    // scale: OPT_FLOAT 仅 strtod——"auto"/"off"/"4/3" 必须换算为数字
    double s = scale_.load();
    std::string scaleOpt;
    if (s == -1)         scaleOpt = "-1";
    else if (s == 0)     scaleOpt = "0";
    else if (fabs(s - 4.0 / 3.0) < 0.01) scaleOpt = "1.3333";
    else { char buf[16]; snprintf(buf, sizeof buf, "%g", s); scaleOpt = buf; }
    // hwup 按 UUID 选 CUDA 设备：多卡机器（双 N 卡 / CUDA_VISIBLE_DEVICES
    // 重映射）上 CUDA 索引顺序与 Vulkan 枚举顺序无关，靠 0 号会选错卡 →
    // 下游把"另一张卡"的 CUDA 帧交给本卡 VO → 黑屏。
    const std::string hwup = aiCudaUuid_.empty()
                                 ? std::string("@hwup:hwup")
                                 : ("@hwup:hwup=cuda-uuid=" + aiCudaUuid_);
    // 旋转（CUDA 零拷贝）：hwup 之后、vsr/vfg 之前。链上恒有（angle=0
    // =直通），运行期 vf-command 只改实例选项、不重建链。
    const std::string rot = "@rot:rot=angle=" + std::to_string(rotMode_.load());
    const std::string vsr = "@vsr:vsr:scale=" + scaleOpt
                          + ":denoise=" + denoiseString()
                          + ":quality=" + qualityString();
    const int fruc = frucFps_.load();
    // SDR→HDR10 上变换（VFX TrueHDR）：**恒在 vsr 之后**（方案 §8.1：VSR 输入
    // 保持今天的 SDR 分布不变）。链串里恒 `enable=no`——实际状态由 HDR 策略经
    // `vf-command truehdr enable|luminance` 运行期下发（单一事实源）。
    // ⚠ enable 是 OPT_BOOL：**只认 yes/no**（"0"/"1" 会让整条 vf 选项解析失败
    // → 链上所有滤镜都不生效，实测）。
    // 峰值取"生效值"（覆盖开关打开时是用户值，否则面板自动值/默认 650）；
    // 显示信息到达后策略会经 vf-command 纠正。参数里只有 luminance 进链串
    //（其余 5 项由 syncVfOptions 重放，链串默认值 = SDK 默认）。
    const std::string up = "@truehdr:truehdr:enable=no:luminance="
                         + std::to_string(effectiveUpHdrLuminance());
#ifdef FRUC_BACKEND_VFG
    // VFG 倍率语义：fps=off|2|3|4 直通/整数倍率；mode 恒发显式值
    // low|medium|high（默认 medium——依据见 FrucBackend.h kModeDefault；
    // 滤镜的 auto 仅作兜底，客户端不再发送）。max-fps = 输出帧率上限
    //（48|60|120，默认 120）：k×src_fps 超过它 → 滤镜直通。
    const std::string vfg = std::string("@") + frucFilterName() + ":" + frucFilterName()
                          + ":fps=" + (fruc > 0 ? std::to_string(fruc) : "off")
                          + ":mode=" + fruc::modeName(frucQuality_.load())
                          + ":max-fps=" + std::to_string(frucMaxFps_.load());
    if (frucOrderPre()) {
        // pre 序下"vsr 之后"= 链尾 = 按输出帧计费（k× 成本）→ 本轮不支持
        // （方案 §8.1）：不插入，日志一次。策略侧同步关掉上变换
        //（initTrueHdr → hdr::setUpconvertInChain），OSD 不会声称在跑。
        static bool pre_warned = false;
        if (!pre_warned) {
            pre_warned = true;
            fprintf(stderr, "[truehdr] upconversion is not supported with --fruc-order pre "
                            "(design §8.1); the truehdr filter is not inserted in this order\n");
        }
        return hwup + "," + rot + "," + vfg + "," + vsr;
    }
    return hwup + "," + rot + "," + vsr + "," + up + "," + vfg;
#else
    std::string vf = hwup + "," + rot + ",";
    if (fruc == 2 || fruc == 3 || fruc == 4) {
        vf += std::string("@") + frucFilterName() + ":" + frucFilterName()
            + ":fps=off:scale=" + std::to_string(fruc) + ",";
    } else if (fruc > 0) {
        vf += std::string("@") + frucFilterName() + ":" + frucFilterName()
            + ":fps=" + std::to_string(fruc) + ":scale=off,";
    } else {
        vf += std::string("@") + frucFilterName() + ":" + frucFilterName()
            + ":fps=off:scale=off,";
    }
    // RIFE 后端：链序 hwup → rife → vsr（rife 在源分辨率），上变换插链尾
    //（= vsr 之后，与 VFG post 序同语义）。
    vf += vsr + "," + up;
    return vf;
#endif
}

std::string PlayerViewModel::scaleString() const { return scaleStr(); }
std::string PlayerViewModel::qualityString() const { return qualityStr(); }
std::string PlayerViewModel::denoiseString() const { return denoiseStr(); }

std::string PlayerViewModel::scaleStr() const {
    double s = scale_.load();
    if (s == -1) return "off";
    if (s == 0)  return "auto";
    if (fabs(s - 4.0 / 3.0) < 0.01) return "4/3";   // filter parse 认识
    if (fabs(s - 1.5) < 0.01) return "1.5";
    char buf[16];
    snprintf(buf, sizeof buf, "%g", s);
    return buf;
}

std::string PlayerViewModel::qualityStr() const {
    switch (quality_) {
    case 1: return "low";
    case 2: return "medium";
    case 4: return "ultra";
    default: return "high";
    }
}

std::string PlayerViewModel::denoiseStr() const {
    switch (denoise_) {
    case 8:  return "low";
    case 9:  return "medium";
    case 10: return "high";
    case 11: return "ultra";
    default: return "off";
    }
}

void PlayerViewModel::pushVf(const char *filter, const char *param,
                             const std::string &value) {
    if (!mpv_) return;
    // label 无 @ 前缀——mpv 的 filter label 匹配不含 @（实测 "@vsr" 不
    // 匹配；RpcServer 的 vf-command 同用法）。命令在调用时同步复制，
    // 无需保活。
    mpv_->commandAsync({"vf-command", filter, param, value.c_str(), nullptr});
}

// FRUC 滤镜名的单一出口（其余地方一律经此取用，见 FrucBackend.h）。
const char *PlayerViewModel::frucFilterName() const { return fruc::kFilterName; }

void PlayerViewModel::setUpconvert(bool on, float autoNit, float effectiveNit) {
    // 口径全部来自 HdrOutput（策略单一事实源）：
    //   autoNit      = 面板物理峰值（合成器 preferred maxLuma；OSD 显示用）
    //   effectiveNit = 实际生效峰值（覆盖开关打开时是用户值，否则 = autoNit）
    // 这里再兜一次区间，避免非法值把 vf-command 顶掉。
    using namespace truehdr_params;
    if (!(autoNit >= kLuminanceMin && autoNit <= kLuminanceMax))
        autoNit = (float)kLuminanceDefault;
    if (!(effectiveNit >= kLuminanceMin && effectiveNit <= kLuminanceMax))
        effectiveNit = autoNit;

    bool changed = false;
    if (on != upconvertOn_) {
        upconvertOn_ = on;
        changed = true;
        emit upHdrEffectiveChanged();
    }
    if (fabsf(autoNit - upHdrAutoNits_) >= 0.5f) {
        upHdrAutoNits_ = autoNit;
        changed = true;
        emit upHdrAutoLuminanceChanged();
    }
    if (fabsf(effectiveNit - upHdrEffectiveNits_) >= 0.5f) {
        upHdrEffectiveNits_ = effectiveNit;
        changed = true;
    }
    if (!changed)
        return;   // 幂等（策略重算会反复触发）

    MLOG_INFO("truehdr: upconversion %s (panel peak %.0f nit, effective peak %.0f nit)",
              on ? "on" : "off", (double)autoNit, (double)effectiveNit);
    // 链上恒有 truehdr 滤镜（vfOption 里 enable=no 占位）→ 运行期开关只改
    // filter 实例选项，不重建链；引擎按需 lazy init（首帧 ~120ms）。
    pushVf("truehdr", "enable", on ? "1" : "0");
    pushVf("truehdr", "luminance", std::to_string((int)lroundf(effectiveNit)));
}

// ── TrueHDR 参数：UI/RPC 入口 + 下发 + 持久化 ─────────────────────────────

int PlayerViewModel::effectiveUpHdrLuminance() const {
    return (int)lroundf(upHdrEffectiveNits_);
}

QVariantMap PlayerViewModel::upHdrLimits() const {
    using namespace truehdr_params;
    QVariantMap m;
    m["lumMin"] = kLuminanceMin;   m["lumMax"] = kLuminanceMax;
    m["lumDef"] = kLuminanceDefault;
    m["satMin"] = kSaturationMin;  m["satMax"] = kSaturationMax;
    m["satDef"] = kSaturationDefault;
    m["conMin"] = kContrastMin;    m["conMax"] = kContrastMax;
    m["conDef"] = kContrastDefault;
    m["mgMin"]  = kMiddleGrayMin;  m["mgMax"]  = kMiddleGrayMax;
    m["mgDef"]  = kMiddleGrayDefault;
    return m;
}

void PlayerViewModel::pushTrueHdrParams() {
    if (!mpv_) return;
    pushVf("truehdr", "saturation", std::to_string(upHdrSaturation_));
    pushVf("truehdr", "contrast", std::to_string(upHdrContrast_));
    pushVf("truehdr", "middle-gray", std::to_string(upHdrMiddleGray_));
    // UI 语义"抗色带开" = SDK `DebandingOff` 0（反相；默认开，铁律 6）
    pushVf("truehdr", "debanding-off", upHdrDebanding_ ? "0" : "1");
    pushVf("truehdr", "luminance", std::to_string(effectiveUpHdrLuminance()));
}

void PlayerViewModel::initTrueHdr() {
    upHdrEnabled_ = persistUpHdrEnabled_;
    upHdrLumOverride_ = persistUpHdrLumOverride_;
    upHdrLuminance_ = persistUpHdrLuminance_;
    upHdrSaturation_ = persistUpHdrSaturation_;
    upHdrContrast_ = persistUpHdrContrast_;
    upHdrMiddleGray_ = persistUpHdrMiddleGray_;
    upHdrDebanding_ = persistUpHdrDebanding_;
    // 显示信息还没到 → 生效值先按"覆盖打开就用用户值，否则 SDK 默认 650"，
    // 策略随后经 setUpconvert 回调纠正为面板峰值。
    upHdrEffectiveNits_ = upHdrLumOverride_ ? (float)upHdrLuminance_
                                            : (float)truehdr_params::kLuminanceDefault;
    // 通知策略（opt-in + 峰值覆盖 + 链上是否有滤镜）：installPolicy 之前调用
    // 只是存值；策略首次 reevaluate 会用到，之后每次变化都会重新决策 + 回调解钩子。
    // `--fruc-order pre`（仅 VFG 可选）下 vfOption() 不插 truehdr（方案 §8.1）→
    // 上变换不可能发生，OSD 的内容标记必须跟着关掉（否则声称 SDR → HDR10 却
    // 没有帧被处理）。frucOrderPre 只由 CLI 决定，此刻已是终值。
    hdr::setUpconvertInChain(!frucOrderPre());
    hdr::setUpconvertOptIn(upHdrEnabled_);
    hdr::setUpconvertLuminanceOverride(upHdrLumOverride_, upHdrLuminance_);
    MLOG_INFO("truehdr: params = %s, saturation=%d contrast=%d middle-gray=%d "
              "debanding=%s, peak=%s", upHdrEnabled_ ? "on" : "off",
              upHdrSaturation_, upHdrContrast_, upHdrMiddleGray_,
              upHdrDebanding_ ? "on" : "off(diagnostic)",
              upHdrLumOverride_ ? "user override" : "auto (panel)");
}

void PlayerViewModel::setUpHdrEnabled(bool on) {
    if (on && !aiAllowed("truehdr")) return;
    if (upHdrEnabled_ == on) return;
    upHdrEnabled_ = on;
    emit upHdrEnabledChanged();
    saveSettings("upHdrEnabled", on);
    MLOG_INFO("truehdr: user switch -> %s", on ? "on" : "off");
    // 策略重算 → 目标模式可能随之切换（SDR 内容 × HDR 屏 × opt-in → Hdr10），
    // 最终 enable 由策略回调 setUpconvert 下发。
    hdr::setUpconvertOptIn(on);
}

void PlayerViewModel::setUpHdrLuminanceOverride(bool on) {
    if (upHdrLumOverride_ == on) return;
    upHdrLumOverride_ = on;
    // 打开覆盖时把滑块**种子**成当前自动值：避免"一开覆盖画面就跳"
    //（用户拖动前不改变生效值）。
    if (on && upHdrAutoNits_ >= truehdr_params::kLuminanceMin)
        upHdrLuminance_ = (int)lroundf(upHdrAutoNits_);
    emit upHdrLuminanceOverrideChanged();
    emit upHdrLuminanceChanged();
    saveSettings("upHdrLumOverride", on);
    saveSettings("upHdrLuminance", upHdrLuminance_);
    // 声明里的内容峰值也要跟着变（HdrOutput 单一事实源）→ 它会回调钩子
    hdr::setUpconvertLuminanceOverride(on, upHdrLuminance_);
}

void PlayerViewModel::setUpHdrLuminance(int nits) {
    using namespace truehdr_params;
    nits = clampInt(nits, kLuminanceMin, kLuminanceMax);
    if (upHdrLuminance_ == nits) return;
    upHdrLuminance_ = nits;
    emit upHdrLuminanceChanged();
    saveSettings("upHdrLuminance", nits);
    if (!upHdrLumOverride_) return;   // 覆盖关：值只暂存，不生效（也不下发）
    hdr::setUpconvertLuminanceOverride(true, nits);
}

void PlayerViewModel::setUpHdrSaturation(int v) {
    using namespace truehdr_params;
    v = clampInt(v, kSaturationMin, kSaturationMax);
    if (upHdrSaturation_ == v) return;
    upHdrSaturation_ = v;
    emit upHdrSaturationChanged();
    saveSettings("upHdrSaturation", v);
    pushVf("truehdr", "saturation", std::to_string(v));
}

void PlayerViewModel::setUpHdrContrast(int v) {
    using namespace truehdr_params;
    v = clampInt(v, kContrastMin, kContrastMax);
    if (upHdrContrast_ == v) return;
    upHdrContrast_ = v;
    emit upHdrContrastChanged();
    saveSettings("upHdrContrast", v);
    pushVf("truehdr", "contrast", std::to_string(v));
}

void PlayerViewModel::setUpHdrMiddleGray(int v) {
    using namespace truehdr_params;
    v = clampInt(v, kMiddleGrayMin, kMiddleGrayMax);
    if (upHdrMiddleGray_ == v) return;
    upHdrMiddleGray_ = v;
    emit upHdrMiddleGrayChanged();
    saveSettings("upHdrMiddleGray", v);
    pushVf("truehdr", "middle-gray", std::to_string(v));
}

void PlayerViewModel::setUpHdrDebanding(bool on) {
    if (upHdrDebanding_ == on) return;
    upHdrDebanding_ = on;
    emit upHdrDebandingChanged();
    saveSettings("upHdrDebanding", on);
    MLOG_INFO("truehdr: debanding -> %s (on by default; disabling exposes banding on "
              "affected shots; engine cost is roughly 1.6-4.3x)", on ? "on" : "off");
    pushVf("truehdr", "debanding-off", on ? "0" : "1");
}

void PlayerViewModel::syncVfOptions() {
    if (!mpv_) return;
    if (!aiAvailable_.load()) return;   // 链上没插滤镜，vf-command 无处可发
    int fruc = frucFps_.load();
    // VFG 倍率模式与 RIFE benchmark 倍率（CLI 2/3/4）都只经 vfOption 配置
    // （fps/scale 参数），此处重放 fps 值即可（RIFE benchmark 分支忽略
    // fps，无破坏；VFG 的 fps 即倍率）；VFG 的 mode 同理由 vfOption 初始
    // 化，链重建后一并重放（RIFE 无此参数，不发送）。
    pushVf(frucFilterName(), "fps", fruc == -1 ? "off" : std::to_string(fruc));
    // 质量档（仅 VFG 有此参数；RIFE 无 mode——发送会得到 vf-command 失败）。
    if (fruc::kModeSelectable)
        pushVf(frucFilterName(), "mode", fruc::modeName(frucQuality_.load()));
    // 输出帧率上限（仅 VFG；理由同上）。
    if (fruc::kMaxFpsSelectable)
        pushVf(frucFilterName(), "max-fps", std::to_string(frucMaxFps_.load()));
    pushVf("vsr", "scale", scaleStr());
    pushVf("vsr", "quality", qualityStr());
    pushVf("vsr", "denoise", denoiseStr());
    // SDR→HDR10 上变换（VFX TrueHDR）：vf-command 只改 filter 实例 opts，
    // 链重建（stop/换片）后回到链串默认 enable=no → 必须重放策略状态，
    // 否则"上变换开了但重播就没了"。
    pushVf("truehdr", "enable", upconvertOn_ ? "1" : "0");
    pushTrueHdrParams();   // 5 个参数全量重放（链重建后回到链串默认值）
    // 旋转：链重建后回到链串里的 angle（初始值 = 持久化值，正常情况
    // 无需重放；这里仍推一次，保证运行期改过角度后换片不丢）。
    pushVf("rot", "angle", std::to_string(rotMode_.load()));
    MLOG_INFO("syncVfOptions: truehdr enable=%d lum=%d sat=%d contrast=%d mg=%d deband=%d",
              int(upconvertOn_), effectiveUpHdrLuminance(), upHdrSaturation_,
              upHdrContrast_, upHdrMiddleGray_, int(upHdrDebanding_));
}

// ── 主线程状态更新（值由事件线程传入，不调 mpv API）─────────────────

void PlayerViewModel::updatePlaying(bool p) {
    if (playing_ != p) { playing_ = p; emit playingChanged(); }
}

void PlayerViewModel::notifyFrameRendered() {
    // 暂停时不计数：暂停时 mpv 的 OSD 重绘/重复帧也产生 uf>0——
    // 计数会与 osd-msg1 推送形成自激（推送→渲染→计数→文本变→推送，
    // 实测暂停时 rendered 每秒 +10 虚增）。playing_ 乐观更新（主线程
    // 立即翻转），暂停瞬间即停计。
    if (!playing_) return;
    renderedFrames_.fetch_add(1);
}

void PlayerViewModel::resetSegmentCounters(int64_t dropBase) {
    renderedFrames_.store(0);
    droppedBase_.store(dropBase);
    fpsPrevRendered_ = 0;          // 段重置 → 下次 tick 差值从 0 起算
    emit osdDataChanged();
}

void PlayerViewModel::updateTime(int64_t t, int64_t d) {
    if (t != currentTime_.load()) { currentTime_.store(t); emit currentTimeChanged(); }
    if (d != duration_.load()) { duration_.store(d); emit durationChanged(); }
}

void PlayerViewModel::updateVolume(double v) {
    if (volume_ != v) { volume_ = v; emit volumeChanged(); }
}

void PlayerViewModel::updateSpeed(double s) {
    if (speed_.load() != s) { speed_.store(s); emit speedChanged(); }
}

void PlayerViewModel::updateHwDecoding(bool hw) {
    if (hwDecoding_.load() != hw) {
        hwDecoding_.store(hw);
        MLOG_INFO("decoder: actual %s -> %s (preference=%s hwdec=%s)",
                  hw ? "software" : "hardware", hw ? "hardware" : "software",
                  hwDecodeEnabled_.load() ? "hardware" : "software",
                  hwdecInit_.empty() ? "?" : hwdecInit_.c_str());
        emit hwDecodingChanged();
    }
}

// 当前文件能否硬解：
//   - 用户偏好 = 软解（hwDecodeEnabled_=false）→ 视为"可切换"（随时能切回
//     硬解，只是这个文件能不能硬解要切了才知道）
//   - 解码器尚未就绪（decoderName_ 空）→ 保持原值，避免加载瞬间误禁用
//   - 偏好=硬解、解码器已就绪、但 hwdec-current = no → **mpv 回退了**，
//     即该文件只能软解 → false（UI 显示"软解"并禁用切换按钮）
void PlayerViewModel::refreshHwDecodeAvailability(int forceReason) {
    bool avail;
    int reason = 0;                      // 0=可切换 1=设备层 2=文件层
    if (!hwDecodeEnabled_.load())        // 用户偏好=软解 → 随时可切回硬解
        avail = true;
    else if (decoderName_.isEmpty())     // 解码器还没起来（加载中）→ 保持原值
        return;
    else {                               // 偏好=硬解却实际软解 → 不能硬解
        avail = hwDecoding_.load();
        if (!avail)
            // 原因分层：**设备**没有可用 CUDA（与我们 AI 可用性同源）优先于
            // **文件** codec 不支持——否则 h264 这种明明能硬解的也会被说成
            // "该文件只能软解"，徒增困惑。
            reason = forceReason ? forceReason : (aiAvailable_.load() ? 2 : 1);
    }
    if (hwDecodeAvailable_.load() != avail || hwDecodeReason_.load() != reason) {
        hwDecodeAvailable_.store(avail);
        hwDecodeReason_.store(reason);
        MLOG_INFO("decoder: decode toggle %s (preference=%s actual=%s decoder=%s)",
                  avail ? "available"
                        : (reason == 1 ? "disabled: this device cannot hardware-decode (no CUDA)"
                                       : "disabled: this file is software-only"),
                  hwDecodeEnabled_.load() ? "hardware" : "software",
                  hwDecoding_.load() ? "hardware" : "software",
                  decoderName_.isEmpty() ? "-" : decoderName_.toUtf8().constData());
        emit hwDecodeAvailableChanged();
    }
}

void PlayerViewModel::updateVideoInfo(const QString &f) {
    if (f.isEmpty()) return;
    if (videoInfo_ != f) { videoInfo_ = f; emit videoInfoChanged(); }
}

void PlayerViewModel::updateLoopMode(int m) {
    if (loopMode_ != m) { loopMode_ = m; emit loopModeChanged(); }
}

void PlayerViewModel::updateLoopModeFromEventThread() {
    if (!mpv_) return;
    std::string lf = mpv_->propertyString("loop-file");
    std::string lp = mpv_->propertyString("loop-playlist");
    int m = (lf == "inf") ? 1 : (lp == "inf") ? 2 : 0;
    QMetaObject::invokeMethod(this, [this, m] { updateLoopMode(m); },
                              Qt::QueuedConnection);
}

void PlayerViewModel::onFileLoadedFromEventThread() {
    if (!mpv_) return;
    std::string p = mpv_->propertyString("path");
    bool paused = mpv_->propertyFlag("pause");
    int64_t drops = mpv_->propertyInt64("frame-drop-count");
    // 直播/线性流：不可 seek 或仅部分可 seek → resume 无意义（见 isLiveSource）
    const bool linear = !mpv_->propertyFlag("seekable")
                     || mpv_->propertyFlag("partially-seekable");
    QMetaObject::invokeMethod(this, [this, p, paused, drops, linear] {
        fileLoaded_ = true;
        idlePlayIndex_ = -1;   // 已有活动文件 → idle 的"待载入索引"作废
        // 按文件记忆：即时落盘保证旧文件状态已随动作写盘，无需在此保存。
        // external 轨随文件切换被 mpv 清除——pending 待确认列表作废
        //（其内容已随 sub-add 动作落盘，此处只防陈旧路径并入新文件）。
        pendingSubs_.clear();
        if (!p.empty())
            lastPath_ = QString::fromStdString(p);
        // 每次加载完成重新同步 playing = !pause。path 在曲目切换时
        // 会临时清空（误触发 updatePlaying(false)），而 pause 值未变
        // 时 mpv 不通知——此回调保证 UI 状态必然收敛到真实值。
        updatePlaying(!paused);
        resetSegmentCounters(drops);   // 新文件 → 段统计归零
        scanSubtitleFiles(lastPath_);  // 新文件 → 扫描其目录的字幕
        if (liveSourcePending_) {      // 检测早于本次加载 → 此刻归属到该文件
            liveSourcePath_ = lastPath_;
            liveSourcePending_ = false;
        }
        if (linear)
            markLiveSource("not seekable");
        else if (mpv_)
            mpv_->setPropertyFlagAsync("save-position-on-quit", true);   // 普通文件恢复位置记忆
        restoreTrackMemory(lastPath_); // 按文件恢复轨道/字幕记忆（再次打开对应文件才恢复）
        syncVfOptions();               // 链重建后重放运行时 vf 状态（fps/vsr 防 UI 修改丢失）
        refreshHwDecodeAvailability(); // 换片后重算"该文件能否硬解"（软解回退不改偏好）
        applyDeinterlace();            // 覆盖 watch_later 里按文件记的 deinterlace
                                       //（mpv 把它列在 watch_later_options → 换片时会
                                       //  用该文件上次的值复位，全局开关必须盖回去）
    }, Qt::QueuedConnection);
}

// ── OSD 文本合成（主线程 1s 周期 pull；纯本地数据）──────────────────
// 渲染走 mpv 内部 OSD（osd-msg1，benchmark 同方案）——OSD 在 mpv VO
// 合成阶段画进视频帧，不经过 Qt 场景图，不竞争主线程渲染循环
//（QML 文本每秒重绘曾使渲染链抖动 → VO 误判 drop 每秒 +1）。
// setPropertyStringAsync：异步投递，不阻塞、不拿 core lock。

/// OSD 文本计算属性（事件线程 idle 回调拉取；纯本地读，零 mpv API）。
/// 数据源 = viewModel 状态（观察器填充）——OSD 显示真实数据且与 UI
/// 同源。rendered/dropped 段内语义保留；渲染 fps = Δrendered/Δt。
std::string PlayerViewModel::osdTextString() {
    QStringList lines;
    // 标签走翻译（Source→源 等，.ts 提供）。对齐按**显示宽度**补空格：
    // 非 Latin-1 字符（中文）monospace 下约占 2 个英文字符宽——按字符数
    // leftJustified 会错位（"源"+7 空格 ≠ "解码器"+5 空格）。
    // ⚠ `tr()` 的参数必须是**字面量**，否则 lupdate 扫不到：历史上这里写成
    // `tr(t)`（t 是形参）→ 所有标签条目被 lupdate 标成 `type="vanished"` →
    // lrelease 不把它们编进 .qm → 运行时 tr() 查不到 → **整个 OSD 标签全英文**
    //（2026-10-18 实测：lconvert 反查 .qm 里没有 <source>Source</source>）。
    // 因此标签在**调用点** tr("Source")，本 lambda 只负责宽度对齐。
    auto tag = [](const QString &label) {
        QString s = label;
        int w = 0;
        for (QChar c : s)
            w += (c.unicode() > 0xFF) ? 2 : 1;
        if (w < 8)
            s += QString(8 - w, ' ');
        return s;
    };

    // 除 label 外 OSD 统一不翻译（用户拍板）：状态行（vsr-status、插帧
    // 状态行 vfg-status/fruc-status 的 mode=/reason=/数值参数）原样英文
    // 显示——翻译会引入语义错位（如 mode=active 是 FRUC 的"插帧中"，
    // vsr-status 共用后超分行误显示）；设定段（auto/Denoise/off 等）同样
    // 保持英文。label 仍走 tr()。

    // Source：源信息（解码尺寸/codec/色深/容器帧率/总帧数）；无视频显示占位
    if (videoWidth_.load() <= 0) {
        lines << tag(tr("Source")) + QStringLiteral("–");
    } else {
        double fps = videoFps_.load();
        QString fpsStr = fps > 0 ? QString::number(fps, 'f', 2)
                                 : QStringLiteral("–");
        int bd = videoBitDepth_.load();
        QString bdStr = bd > 0 ? QStringLiteral("%1bit").arg(bd)
                               : QStringLiteral("–");
        lines << tag(tr("Source")) + QStringLiteral("%1×%2 %3 %4 %5fps · %6f")
            .arg(videoWidth_.load()).arg(videoHeight_.load())
            .arg(videoCodec_).arg(bdStr).arg(fpsStr).arg(decodedFrames_.load());
    }

    // Output：视频视口物理尺寸 + 实际渲染视频帧率（Δt 实测差值）
    {
        qint64 dtMs = fpsTimer_.isValid() ? fpsTimer_.restart()
                                          : (fpsTimer_.start(), (qint64)0);
        int64_t rendered = renderedFrames_.load();
        if (dtMs <= 0) {
            renderFps_.store(0.0);   // 首拉取（打开/重置后）无 Δt
        } else {
            renderFps_.store((rendered - fpsPrevRendered_) * 1000.0 / dtMs);
        }
        fpsPrevRendered_ = rendered;
        QString out = QStringLiteral("%1×%2 %3fps")
            .arg(viewportWidth_.load()).arg(viewportHeight_.load())
            .arg(renderFps_.load(), 0, 'f', 1);
        // 实际输出模式（HDR10/SDR，来自 hdr::outputText() = 真实 swapchain 格式）
        // 并入本行——语义 = "输出到显示的是什么"（HDR 行讲的是内容侧状态）。
        {
            std::lock_guard<std::mutex> lk(hdrOutputMtx_);
            if (!hdrOutput_.empty())
                out += QStringLiteral(" · ") + QString::fromStdString(hdrOutput_);
        }
        lines << tag(tr("Output")) + out;
    }

    // HDR：上屏状态（SDR / HDR10 + 显示峰值 + 内容峰值/未上屏原因）。
    // 文本由 hdr::statusText() 给出（英文，不翻译），平台无关地反映实际状态。
    {
        std::lock_guard<std::mutex> lk(hdrStatusMtx_);
        lines << tag(tr("HDR")) + QString::fromStdString(hdrStatus_);
    }

    // VSR：设定 | 状态（原 Render 行合并：输出帧尺寸——倍率由设定段
    // 已表达，移除重复显示；vsr-status 并入同一状态段）。
    {
        QStringList parts;
        double scale = scale_.load();
        int quality = quality_.load();
        int denoise = denoise_.load();
        if (scale > 1)
            parts << QStringLiteral("%1× %2").arg(scaleStr().c_str())
                     .arg(qualityName(quality));
        else if (scale == 0)
            parts << QStringLiteral("auto %1").arg(qualityName(quality));
        if (denoise != -1)
            parts << QStringLiteral("Denoise %1").arg(denoiseName(denoise));
        QStringList st;   // 状态段：输出尺寸 + vsr-status
        int renderW = renderWidth_.load();
        if (renderW > 0)
            st << QStringLiteral("%1×%2").arg(renderW).arg(renderHeight_.load());
        // vsr 关闭（scale=-1）时不并入 vsr-status——filter 在链中但无
        // 处理，status 是切换瞬间的旧值残留（cost/fps 无意义）
        if (scale > -0.5) {
            std::lock_guard<std::mutex> lk(vsrStatusMtx_);
            if (!vsrStatus_.empty())
                st << QString::fromStdString(vsrStatus_);
        }
        QString line = tag(tr("VSR")) +
            (parts.isEmpty() ? QStringLiteral("off") : parts.join(" "));
        // AI 不可用（本卡没有匹配的 CUDA 设备）：设定值仍然显示，但必须
        // 明确标注没在跑——否则 OSD 会声称在做超分而链上根本没插滤镜。
        if (!aiAvailable_.load() && !parts.isEmpty())
            line += QStringLiteral(" | AI unavailable");
        else if (!st.isEmpty())
            line += " | " + st.join(" ");
        lines << line;
    }

    // FRUC：设定（目标帧率/倍率，off 显示"off"——整行不删除）|
    // 实际状态（滤镜的插帧状态行，事件线程按后端前缀提取——
    // VFG "vfg-status:" / RIFE "fruc-status:"，见 FrucBackend.h；off 时
    // 无状态段）。设定段语义随编译分支：VFG 只有倍率档（-1/2/3/4 →
    // "off"/"K×"），RIFE 保持目标帧率档（40/48/60 → "N fps"）。
    {
        int fruc = frucFps_.load();
        QString target;
        if (fruc == -1)
            target = QStringLiteral("off");
        else if (fruc == 2 || fruc == 3 || fruc == 4)
            target = QStringLiteral("%1×").arg(fruc);
        else if (fruc > 0)
            target = QStringLiteral("%1 fps").arg(fruc);
        QString line = tag(tr("FRUC")) + target;
        if (!aiAvailable_.load() && fruc != -1) {
            line += QStringLiteral(" | AI unavailable");
        } else if (fruc != -1) {
            std::lock_guard<std::mutex> lk(frucStatusMtx_);
            if (!frucStatus_.empty())
                line += " | " + QString::fromStdString(frucStatus_);
        }
        lines << line;
    }

    // 硬解显示实际格式（hw-pixelformat，如 p010/nv12）——pixelformat
    // 是 cuda 包装名无信息量
    // Decoder：实际解码器——软解输出解码器名（libdav1d 等），硬解输出
    // codec + hwdec API（av1 nvdec）+ 像素格式。解码器名不可用时兜底
    // 软解/硬解标签。
    if (hwDecoding_.load()) {
        QString hd = hwdecName_;
        hd.remove("-copy");   // nvdec-copy → nvdec（copy 模式无信息价值）
        lines << tag(tr("Decoder")) + QStringLiteral("%1 %2 %3")
            .arg(decoderName_.isEmpty() ? QStringLiteral("NVDEC") : decoderName_,
                 hd.isEmpty() ? QStringLiteral("NVDEC") : hd,
                 hwPixelFormat_.isEmpty() ? decoderPixelFormat_ : hwPixelFormat_);
    } else {
        lines << tag(tr("Decoder")) + QStringLiteral("%1 %2")
            .arg(decoderName_.isEmpty() ? QStringLiteral("Software") : decoderName_,
                 hwPixelFormat_.isEmpty() ? decoderPixelFormat_ : hwPixelFormat_);
    }

    lines << tag(tr("Speed")) + QStringLiteral("%1×").arg(speed_.load(), 0, 'f', 2);

    lines << tag(tr("Time")) + QStringLiteral("%1 / %2")
        .arg(fmtTime(currentTime_.load())).arg(fmtTime(duration_.load()));

    // Frames：段内统计（rendered 客户端计数 / dropped 差值）
    lines << tag(tr("Frames")) + QStringLiteral("rendered %1  dropped %2")
        .arg(renderedFrames_.load()).arg(droppedFrames());

    if (!gpuName_.isEmpty())
        lines << tag(tr("GPU")) + gpuName_;

    if (audioSampleRate_.load() > 0)
        lines << tag(tr("Audio")) + QStringLiteral("%1Hz %2ch")
            .arg(audioSampleRate_.load()).arg(audioChannels_.load());

    return lines.join('\n').toStdString();
}

/// OSD 拉取推送（事件线程 idle 回调，~100ms）：osdVisible 时拉取文本
/// 并推送 osd-msg1/osd-level。事件线程调 mpv API 安全（线程安全；
/// 主线程调 osd-msg1 更新会与 untimed 渲染死锁，实测卡死）。
void PlayerViewModel::osdPoll() {
    if (!mpv_) return;
    bool visible = osdVisible_.load();
    if (visible != osdLevelApplied_) {
        osdLevelApplied_ = visible;
        mpv_->setPropertyString("osd-level", visible ? "1" : "0");
    }
    if (!visible) return;
    std::string text = osdTextString();
    if (text != osdLastText_) {
        osdLastText_ = std::move(text);
        mpv_->setPropertyString("osd-msg1", osdLastText_);
    }
}
