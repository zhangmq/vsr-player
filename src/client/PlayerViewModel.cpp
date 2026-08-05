#include "PlayerViewModel.h"
#include "MpvController.h"

#include <QMetaObject>

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
}

void PlayerViewModel::applyPlaybackSettings() {
    if (!mpv_) return;
    setVolume(settings_.value("volume", 1.0).toDouble());
    setSpeed(settings_.value("speed", 1.0).toDouble());
    setLoopMode(settings_.value("loopMode", 0).toInt());
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
    pushVf("scale", scaleStr());  // 热更新，不重建 filter 链
    bool nowActive = vsrActive();
    if (wasActive != nowActive) emit vsrActiveChanged();
}

void PlayerViewModel::setQuality(int q) {
    if (!mpv_) return;
    if (q < 1 || q > 4) return;
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
    saveSettings("quality", q);
    pushVf("quality", qualityStr());
}

void PlayerViewModel::setDenoiseQuality(int d) {
    if (!mpv_) return;
    if (d != -1 && (d < 8 || d > 11)) return;
    bool wasActive = vsrActive();
    if (denoise_ != d) { denoise_ = d; emit denoiseQualityChanged(); }
    saveSettings("denoiseQuality", d);
    pushVf("denoise", denoiseStr());
    bool nowActive = vsrActive();
    if (wasActive != nowActive) emit vsrActiveChanged();
}

// ── Speed / window / OSD ─────────────────────────────────────────────

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
        for (const QString &p : paths)
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
        return;
    }
    // 混拖分类：字幕文件 → sub-add，其余 → loadfile（混合目录场景）
    bool first = true;
    for (const QString &p : paths) {
        if (isSubtitleFile(p)) {
            mpv_->commandAsync({"sub-add", p.toUtf8().constData(), "select", nullptr});
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
    if (type == "audio")      { mpv_->setPropertyInt64Async("aid", id); return; }
    if (type == "video")      { mpv_->setPropertyInt64Async("vid", id); return; }
    if (type == "sub") {
        if (id >= 0) mpv_->setPropertyInt64Async("sid", id);
        else         mpv_->setPropertyStringAsync("sid", "no");
    }
}

void PlayerViewModel::toggleSubtitles() {
    if (!mpv_) return;
    mpv_->setPropertyFlagAsync("sub-visibility", !subVisible_);
}

void PlayerViewModel::adjustSubDelay(double delta) {
    if (!mpv_) return;
    mpv_->setPropertyDoubleAsync("sub-delay", subDelay_ + delta);
}

void PlayerViewModel::playlistRemove(int index) {
    if (!mpv_) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", index);
    mpv_->commandAsync({"playlist-remove", buf, nullptr});
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
    // scale: OPT_FLOAT 仅 strtod——"auto"/"off"/"4/3" 必须换算为数字
    double s = scale_.load();
    std::string scaleOpt;
    if (s == -1)         scaleOpt = "-1";
    else if (s == 0)     scaleOpt = "0";
    else if (fabs(s - 4.0 / 3.0) < 0.01) scaleOpt = "1.3333";
    else { char buf[16]; snprintf(buf, sizeof buf, "%g", s); scaleOpt = buf; }
    std::string vf = "@vsr:vsr:scale=" + scaleOpt;
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

void PlayerViewModel::pushVf(const char *param, const std::string &value) {
    if (!mpv_) return;
    // label 用 "vsr"（无 @）——mpv 的 filter label 匹配不含 @ 前缀
    //（实测 "@vsr" 不匹配；RpcServer 的 vf-command 同用法）。
    // 命令在调用时同步复制，无需保活。
    mpv_->commandAsync({"vf-command", "vsr", param, value.c_str(), nullptr});
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
        if (!p.empty())
            lastPath_ = QString::fromStdString(p);
        // 每次加载完成重新同步 playing = !pause。path 在曲目切换时
        // 会临时清空（误触发 updatePlaying(false)），而 pause 值未变
        // 时 mpv 不通知——此回调保证 UI 状态必然收敛到真实值。
        updatePlaying(!paused);
        resetSegmentCounters(drops);   // 新文件 → 段统计归零
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

    // VSR：配置状态（auto 也显示 quality；实际倍率由 Render 行显示）
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
        lines << tag("VSR") + (parts.isEmpty() ? tr("off") : parts.join("  "));
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
