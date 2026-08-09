#include "PlayerViewModel.h"
#include "MpvController.h"

#include <QMetaObject>
#include <QVariantMap>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QLocale>

#include <cstdio>
#include <cstring>

namespace {

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

PlayerViewModel::PlayerViewModel(QObject *parent) : QObject(parent) {}

// ── attach: register mpv property observers ────────────────────────────
//
// 线程模型（关键，防 untimed 死锁）：
//  mpv 核心持 core lock 在 flip_page 等 render 消费（200ms 超时）。
//  若主线程（渲染循环所在线程）调用 mpv_get_property 等 client API，
//  会与 core lock 互锁 → UpdateRequest 事件饿死 → 渲染循环停止 →
//  flip_page 超时 drop → 整体 5fps 卡顿。
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
            updatePlaying(!p);
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
            if (!p.empty())
                lastPath_ = QString::fromStdString(p);
        });
    });

    // idle-active（stop_play == PT_STOP）才是"真正停止"的信号：
    // 停止命令 / 播完且无后续条目时置真；曲目切换期间保持假。
    mpv_->observeProperty("idle-active", MPV_FORMAT_FLAG, [this, post] {
        bool idle = mpv_->propertyFlag("idle-active");
        int64_t drops = mpv_->propertyInt64("frame-drop-count");
        post([this, idle, drops] {
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
            playlistModel_.setSnapshot(files, cur);
            // 持久化上次播放列表（启动 restorePlaylist 用；清空也写）
            saveSettings("playlist", files);
            saveSettings("playlistCurrent", cur);
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
            std::string pix, hwPix;
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
            postData([this, w, h, pix, hwPix, bd] {
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
    // 单一字段 frucFps_（-1 off | 2/3/4 倍率 | 30/40/60 目标帧率）——
    // benchmark 与正常模式共用一套状态（OSD/ vf 构造不分叉）。
    if (fruc == "2" || fruc == "3" || fruc == "4" ||
        fruc == "40" || fruc == "48" || fruc == "60") {
        frucFps_ = atoi(fruc.c_str());
        return;
    }
    // 空/非法：benchmark = 不插帧（CLI 语义）；正常 = 持久化目标。
    frucFps_ = benchmark ? -1 : persistFruc_;
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
    if (persistFruc_ != -1 && persistFruc_ != 30 && persistFruc_ != 40 && persistFruc_ != 60)
        persistFruc_ = -1;   // 非法值（手改配置）→ off
}

void PlayerViewModel::applyPlaybackSettings() {
    if (!mpv_) return;
    setVolume(settings_.value("volume", 1.0).toDouble());
    setSpeed(settings_.value("speed", 1.0).toDouble());
    setLoopMode(settings_.value("loopMode", 0).toInt());
    setAspect(settings_.value("aspect", QStringLiteral("no")).toString());
}

void PlayerViewModel::restorePlaylist() {
    if (!mpv_) return;
    QStringList files = settings_.value("playlist").toStringList();
    int cur = settings_.value("playlistCurrent", -1).toInt();
    if (files.isEmpty()) return;
    if (cur < 0 || cur >= files.size()) cur = 0;
    // 首条 replace 开始播放，其余 append 排队；最后定位上次条目。
    // loadfile 全部同步命令（mpv 启动阶段无 core-lock 竞争）。
    mpv_->commandV({"loadfile", files[0].toUtf8().constData(), nullptr});
    for (int i = 1; i < files.size(); i++)
        mpv_->commandV({"loadfile", files[i].toUtf8().constData(), "append", nullptr});
    if (cur != 0)
        mpv_->commandV({"playlist-play-index", std::to_string(cur).c_str(), nullptr});
}

// ── Playback（乐观更新：本地状态立即反映，mpv 回写校正）──────────────

void PlayerViewModel::play() {
    if (!mpv_) return;
    if (!fileLoaded_) resumeLastPath();
    playing_ = true;
    emit playingChanged();
    mpv_->setPropertyFlag("pause", false);
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
    mpv_->setPropertyFlag("pause", true);
}

void PlayerViewModel::setPaused(bool p) {
    if (!mpv_) return;
    if (!p) {
        play();   // idle 时重载 lastPath（与 RPC set_property pause no 语义一致）
        return;
    }
    playing_ = false;
    emit playingChanged();
    mpv_->setPropertyFlag("pause", true);
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
        mpv_->setPropertyFlag("pause", false);
        playing_ = true;
        emit playingChanged();
        return;
    }
    bool p = !playing_;
    playing_ = p;
    emit playingChanged();
    mpv_->setPropertyFlag("pause", !p);
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
    mpv_->commandV({"stop", "keep-playlist", nullptr});
}

void PlayerViewModel::seekAbsolute(int64_t ms) {
    if (!mpv_) return;
    currentTime_.store(ms);
    emit currentTimeChanged();
    // %.3f 保留亚秒精度（ms/1000 整数除会丢 0.5s → "seek 0 absolute" 从头重启）。
    // 参数必须分解传递：commandAsync 不做整行解析（整串会被当 target 解析失败）。
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", ms / 1000.0);
    mpv_->commandAsync({"seek", buf, "absolute", nullptr});   // async：主线程不阻塞 core-lock
}

void PlayerViewModel::seekRelative(int64_t offsetMs) {
    if (!mpv_) return;
    int64_t target = currentTime_ + offsetMs;
    if (target < 0) target = 0;
    if (duration_ > 0 && target > duration_) target = duration_;
    seekAbsolute(target);
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
    // 乐观翻转（mpv 的 hwdec 属性修改只在新文件加载时生效 → replace
    // 重载当前文件保持进度；hwdec-active 观察事件随后回写权威值）
    bool target = !hwDecoding_;
    hwDecoding_.store(target);
    emit hwDecodingChanged();
    std::string path = mpv_->propertyString("path");
    if (path.empty()) return;
    double pos = mpv_->propertyDouble("time-pos");
    if (hwdecInit_.empty())
        hwdecInit_ = mpv_->propertyString("hwdec");
    mpv_->setPropertyString("hwdec", target ? hwdecInit_ : "no");
    char start[64];
    snprintf(start, sizeof(start), "start=+%.3f", pos);
    mpv_->commandV({"loadfile", path.c_str(), "replace", start, nullptr});
}

// ── Video settings ───────────────────────────────────────────────────

void PlayerViewModel::setScale(double s) {
    if (!mpv_) return;
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
    if (q < 1 || q > 4) return;
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
    saveSettings("quality", q);
    pushVf("vsr", "quality", qualityStr());
}

void PlayerViewModel::setDenoiseQuality(int d) {
    if (!mpv_) return;
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
    if (v != -1 && v != 40 && v != 48 && v != 60) return;
    if (frucFps_ != v) { frucFps_ = v; emit frucFpsChanged(); }
    saveSettings("frucFps", v);
    pushVf("rife", "fps", v == -1 ? "off" : std::to_string(v));
}

// 剥离 mpv 日志消息尾随换行（mpv 的 log text 以 \n 结尾——原样存入
// 会让 OSD 行内嵌 \n → 行距异常/提前折行，见 osdTextString）
static void stripEol(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
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

// ── Speed / window / OSD ─────────────────────────────────────────────

void PlayerViewModel::setAspect(const QString &v) {
    if (!mpv_) return;
    if (aspect_ != v) { aspect_ = v; emit aspectChanged(); }
    saveSettings("aspect", v);
    mpv_->setPropertyString("video-aspect-override", v.toStdString());
}

void PlayerViewModel::setSpeed(double speed) {
    if (!mpv_) return;
    if (speed < 0.1) speed = 0.1;
    if (speed > 4.0) speed = 4.0;
    if (speed_.load() != speed) { speed_.store(speed); emit speedChanged(); }
    saveSettings("speed", speed);
    mpv_->setPropertyDouble("speed", speed);
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
        mpv_->setPropertyString("loop-file", "inf");
        mpv_->setPropertyString("loop-playlist", "no");
        break;
    case 2:
        mpv_->setPropertyString("loop-file", "no");
        mpv_->setPropertyString("loop-playlist", "inf");
        break;
    default:
        mpv_->setPropertyString("loop-file", "no");
        mpv_->setPropertyString("loop-playlist", "no");
        break;
    }
    if (loopMode_ != m) { loopMode_ = m; emit loopModeChanged(); }
    saveSettings("loopMode", m);
}

void PlayerViewModel::toggleLoop() {
    if (!mpv_) return;
    setLoopMode((loopMode_ + 1) % 3);
}

void PlayerViewModel::playlistNext() { if (mpv_) mpv_->commandStr("playlist-next"); }
void PlayerViewModel::playlistPrev() { if (mpv_) mpv_->commandStr("playlist-prev"); }

void PlayerViewModel::playlistPlayIndex(int index) {
    if (!mpv_) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "playlist-play-index %d", index);
    mpv_->commandStr(buf);
}

// ── File ─────────────────────────────────────────────────────────────

void PlayerViewModel::loadFile(const QString &path) {
    if (!mpv_) return;
    videoInfo_ = path.section('/', -1);
    emit videoInfoChanged();
    mpv_->commandV({"loadfile", path.toUtf8().constData(), nullptr});
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
            if (first) mpv_->commandV({"loadfile", p.toUtf8().constData(), nullptr});
            else       mpv_->commandV({"loadfile", p.toUtf8().constData(), "append", nullptr});
            first = false;
        }
        return;
    }
    if (mode == 3) {   // 打开文件夹：扫描其中媒体文件 → replace + queue（同 mode 0）
        QStringList media;
        for (const QString &p : paths) {
            QFileInfo fi(p);
            if (!fi.isDir()) continue;   // FolderDialog 单选文件夹；防御忽略
            QDir dir(p);
            const QStringList exts = {"*.mp4", "*.mkv", "*.webm", "*.avi",
                                      "*.mov", "*.ts", "*.flv", "*.wmv",
                                      "*.mp3", "*.flac", "*.wav", "*.ogg", "*.m4a"};
            const QStringList names = dir.entryList(exts, QDir::Files, QDir::Name);
            for (const QString &n : names) media.append(dir.filePath(n));
        }
        if (media.isEmpty()) return;
        mpv_->commandV({"loadfile", media[0].toUtf8().constData(), nullptr});
        for (int i = 1; i < media.size(); i++)
            mpv_->commandV({"loadfile", media[i].toUtf8().constData(), "append", nullptr});
        return;
    }
    // 混拖分类：字幕文件 → sub-add，其余 → loadfile（混合目录场景）
    bool first = true;
    for (const QString &p : paths) {
        if (isSubtitleFile(p)) {
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
            noteExternalSubAdded(p);   // 即时落盘（pending 暂存未回写路径）
            continue;
        }
        if (mode == 1 || !first)
            mpv_->commandV({"loadfile", p.toUtf8().constData(), "append", nullptr});
        else
            mpv_->commandV({"loadfile", p.toUtf8().constData(), nullptr});
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
    // 轨道选择（FILE_LOADED 后 track-list 已填充，设置 aid/sid/vid 生效）
    if (mem.contains("aid"))
        mpv_->setPropertyString("aid", std::to_string(mem["aid"].toInt()));
    if (mem.contains("vid"))
        mpv_->setPropertyString("vid", std::to_string(mem["vid"].toInt()));
    if (mem.contains("sid"))
        mpv_->setPropertyString("sid", std::to_string(mem["sid"].toInt()));
    // 字幕偏移（可见性无独立记忆——是否显示由 sid 决定）
    if (mem.contains("subDelay"))
        mpv_->setPropertyDouble("sub-delay", mem["subDelay"].toDouble());
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
    if (best >= 0) { mpv_->setPropertyString("sid", std::to_string(best)); return; }
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
    mpv_->setPropertyString("sid", "no");
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
    mpv_->commandAsync({"playlist-clear", nullptr});
}

void PlayerViewModel::screenshot() {
    if (!mpv_) return;
    mpv_->commandStr("screenshot");
}

// ── vf string ────────────────────────────────────────────────────────

std::string PlayerViewModel::vfOption() const {
    // 链序：decode → rife（插帧，源分辨率）→ vsr（超分）→ VO。
    // rife 目标语义（单一字段 frucFps_，数值自区分）：2/3/4 = 倍率
    //（benchmark 强制，scale+adaptive=no——decide_mode 的 benchmark 分支
    // 跳过全部直通限制）；30/40/60 = 目标帧率（正常模式，adaptive=yes）。
    std::string vf;
    int fruc = frucFps_.load();
    if (fruc == 2 || fruc == 3 || fruc == 4) {
        vf += "@rife:rife:fps=off:scale=" + std::to_string(fruc) + ":adaptive=no,";
    } else if (fruc > 0) {
        vf += "@rife:rife:fps=" + std::to_string(fruc) + ":scale=off:adaptive=yes,";
    } else {
        vf += "@rife:rife:fps=off:scale=off:adaptive=yes,";
    }
    // scale: OPT_FLOAT 仅 strtod——"auto"/"off"/"4/3" 必须换算为数字
    double s = scale_.load();
    std::string scaleOpt;
    if (s == -1)         scaleOpt = "-1";
    else if (s == 0)     scaleOpt = "0";
    else if (fabs(s - 4.0 / 3.0) < 0.01) scaleOpt = "1.3333";
    else { char buf[16]; snprintf(buf, sizeof buf, "%g", s); scaleOpt = buf; }
    vf += "@vsr:vsr:scale=" + scaleOpt;
    vf += ":denoise=" + denoiseString();
    vf += ":quality=" + qualityString();
    return vf;
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
    if (hwDecoding_.load() != hw) { hwDecoding_.store(hw); emit hwDecodingChanged(); }
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
    QMetaObject::invokeMethod(this, [this, p, paused, drops] {
        fileLoaded_ = true;
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
        restoreTrackMemory(lastPath_); // 按文件恢复轨道/字幕记忆（再次打开对应文件才恢复）
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
    auto tag = [](const char *t) {
        QString s = tr(t);
        int w = 0;
        for (QChar c : s)
            w += (c.unicode() > 0xFF) ? 2 : 1;
        if (w < 8)
            s += QString(8 - w, ' ');
        return s;
    };

    // 状态行本地化：mode/reason 关键词翻译（src=/out=/cost= 等数值参数
    // 保留英文缩写——技术参数）。mpv 侧日志英文，显示侧映射（.ts 条目）。
    auto trStatus = [](QString &s) {
        static const char *kw[] = {
            "mode=active", "mode=passthrough",
            "reason=cost", "reason=src-fps", "reason=off",
            "reason=sw", "reason=engine", "reason=engine-size",
            "reason=no-pts",
        };
        for (const char *k : kw)
            s.replace(k, tr(k));
    };

    // Source：源信息（解码尺寸/codec/色深/容器帧率/总帧数）；无视频显示占位
    if (videoWidth_.load() <= 0) {
        lines << tag("Source") + tr("–");
    } else {
        double fps = videoFps_.load();
        QString fpsStr = fps > 0 ? QString::number(fps, 'f', 2)
                                 : QStringLiteral("–");
        int bd = videoBitDepth_.load();
        QString bdStr = bd > 0 ? tr("%1bit").arg(bd)
                               : QStringLiteral("–");
        lines << tag("Source") + tr("%1×%2 %3 %4 %5fps · %6f")
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
        lines << tag("Output") + tr("%1×%2 %3fps")
            .arg(viewportWidth_.load()).arg(viewportHeight_.load())
            .arg(renderFps_.load(), 0, 'f', 1);
    }

    // Render：filter 输出帧尺寸 + 实际生效倍率（renderWidth/videoWidth）
    int renderW = renderWidth_.load();
    if (renderW <= 0) {
        lines << tag("Render") + tr("–");
    } else {
        int videoW = videoWidth_.load();
        double eff = videoW > 0 ? (double)renderW / videoW : 1.0;
        lines << tag("Render") + tr("%1×%2 (%3×)")
            .arg(renderW).arg(renderHeight_.load())
            .arg(eff, 0, 'f', 2);
    }

    // VSR：设定（配置状态，auto 也显示 quality）| 实际状态（vsr-status
    // 合并——处理耗时/频率/直通，| 分隔设定与实际）。
    {
        QStringList parts;
        double scale = scale_.load();
        int quality = quality_.load();
        int denoise = denoise_.load();
        if (scale > 1)
            parts << tr("%1× %2").arg(scaleStr().c_str()).arg(tr(qualityName(quality)));
        else if (scale == 0)
            parts << tr("auto %1").arg(tr(qualityName(quality)));
        if (denoise != -1)
            parts << tr("Denoise %1").arg(tr(denoiseName(denoise)));
        QString line = tag("VSR") + (parts.isEmpty() ? tr("off") : parts.join("  "));
        std::lock_guard<std::mutex> lk(vsrStatusMtx_);
        if (!vsrStatus_.empty()) {
            QString vsrs = QString::fromStdString(vsrStatus_);
            trStatus(vsrs);   // mode/reason 本地化（数值参数保留）
            line += " | " + vsrs;
        }
        lines << line;
    }

    // FRUC：设定（目标帧率/倍率）| 实际状态（rife 的 fruc-status 行，
    // 事件线程从 "fruc-status:" 日志提取；off 时无意义不显示）。
    if (frucFps_.load() != -1) {
        std::lock_guard<std::mutex> lk(frucStatusMtx_);
        int fruc = frucFps_.load();
        QString target;
        if (fruc == 2 || fruc == 3 || fruc == 4)
            target = tr("%1×").arg(fruc);
        else if (fruc > 0)
            target = tr("%1 fps").arg(fruc);
        QString line = tag("FRUC") + target;
        if (!frucStatus_.empty()) {
            QString frucs = QString::fromStdString(frucStatus_);
            trStatus(frucs);
            line += " | " + frucs;
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
        lines << tag("Decoder") + tr("%1 %2 %3")
            .arg(decoderName_.isEmpty() ? tr("NVDEC") : decoderName_,
                 hd.isEmpty() ? tr("NVDEC") : hd,
                 hwPixelFormat_.isEmpty() ? decoderPixelFormat_ : hwPixelFormat_);
    } else {
        lines << tag("Decoder") + tr("%1 %2")
            .arg(decoderName_.isEmpty() ? tr("Software") : decoderName_,
                 hwPixelFormat_.isEmpty() ? decoderPixelFormat_ : hwPixelFormat_);
    }

    lines << tag("Speed") + tr("%1×").arg(speed_.load(), 0, 'f', 2);

    lines << tag("Time") + tr("%1 / %2")
        .arg(fmtTime(currentTime_.load())).arg(fmtTime(duration_.load()));

    // Frames：段内统计（rendered 客户端计数 / dropped 差值）
    lines << tag("Frames") + tr("rendered %1  dropped %2")
        .arg(renderedFrames_.load()).arg(droppedFrames());

    if (!gpuName_.isEmpty())
        lines << tag("GPU") + gpuName_;

    if (audioSampleRate_.load() > 0)
        lines << tag("Audio") + tr("%1Hz %2ch")
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
