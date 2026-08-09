#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>

#include "PlaylistModel.h"

class MpvController;

/// Single source of truth for all UI state.
///
/// QML binds to Q_PROPERTYs; user actions call slots. Slots optimistically
/// update properties before sending commands to mpv (via MpvController).
/// Property observations (mpv_observe_property) flow back through the event
/// thread and are marshalled to the main thread with QueuedConnection —
/// update* methods hold authoritative values.
///
/// Threading: all state lives on the main thread. attach() must be called
/// before MpvController::startEvents (observers are registered there).
class PlayerViewModel : public QObject {
    Q_OBJECT
    // Playback
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(int64_t currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    // Overlay
    Q_PROPERTY(bool overlaysVisible READ overlaysVisible WRITE setOverlaysVisible NOTIFY overlaysVisibleChanged)
    // Audio
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    // 无独立 mute 状态位：volume == 0 即静音（派生属性，volumeChanged 通知）
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    // Video settings
    Q_PROPERTY(int quality READ quality NOTIFY qualityChanged)
    Q_PROPERTY(bool vsrActive READ vsrActive NOTIFY vsrActiveChanged)
    Q_PROPERTY(double scale READ scale NOTIFY scaleChanged)
    Q_PROPERTY(int denoiseQuality READ denoiseQuality NOTIFY denoiseQualityChanged)
    // Frame interpolation (RIFE): -1=off | 30/40/60 target fps (UI presets)
    Q_PROPERTY(int frucFps READ frucFps NOTIFY frucFpsChanged)
    // Speed
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    // Aspect ratio（mpv video-aspect-override：-1=auto, no=不覆盖, 或 16:9 等）
    Q_PROPERTY(QString aspect READ aspect NOTIFY aspectChanged)
    // Info
    Q_PROPERTY(bool hwDecoding READ hwDecoding NOTIFY hwDecodingChanged)
    Q_PROPERTY(QString videoInfo READ videoInfo NOTIFY videoInfoChanged)
    // OSD 数据源（观察器填充，主线程持有）
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY osdDataChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY osdDataChanged)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY osdDataChanged)
    Q_PROPERTY(double videoFps READ videoFps NOTIFY osdDataChanged)
    Q_PROPERTY(int64_t decodedFrames READ decodedFrames NOTIFY osdDataChanged)
    Q_PROPERTY(int renderWidth READ renderWidth NOTIFY osdDataChanged)
    Q_PROPERTY(int renderHeight READ renderHeight NOTIFY osdDataChanged)
    Q_PROPERTY(int viewportWidth READ viewportWidth NOTIFY osdDataChanged)
    Q_PROPERTY(int viewportHeight READ viewportHeight NOTIFY osdDataChanged)
    Q_PROPERTY(QString decoderPixelFormat READ decoderPixelFormat NOTIFY osdDataChanged)
    Q_PROPERTY(int64_t droppedFrames READ droppedFrames NOTIFY osdDataChanged)
    Q_PROPERTY(int audioSampleRate READ audioSampleRate NOTIFY osdDataChanged)
    Q_PROPERTY(int audioChannels READ audioChannels NOTIFY osdDataChanged)
    Q_PROPERTY(double renderFps READ renderFps NOTIFY osdDataChanged)
    Q_PROPERTY(int64_t renderedFrames READ renderedFrames NOTIFY osdDataChanged)
    // Window
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)
    // OSD（mpv 内部 osd-msg1 渲染；osdVisible_ 为内部状态，事件线程读）
    // Q_PROPERTY 无——QML 不再承载 OSD 显示
    // Loop mode: 0=none, 1=file, 2=playlist (mpv loop-file / loop-playlist)
    Q_PROPERTY(int loopMode READ loopMode NOTIFY loopModeChanged)
    // 轨道列表（track-list 观察器填充）/ 字幕可见性/延迟（TracksPopup 数据源）
    Q_PROPERTY(QVariantList trackList READ trackList NOTIFY trackListChanged)
    Q_PROPERTY(bool subVisible READ subVisible NOTIFY subVisibleChanged)
    Q_PROPERTY(double subDelay READ subDelay NOTIFY subDelayChanged)
    // 视频同目录未加载的外部字幕（{name, path}，优先级排序；已加载排除）
    Q_PROPERTY(QVariantList subtitleFiles READ subtitleFiles NOTIFY subtitleFilesChanged)
    // Playlist（mpv `playlist` 属性 → PlaylistModel 增量镜像）。
    // currentIndex 由 PlaylistModel 自带 currentIndexChanged，切歌不再
    // 触发列表 reset（旧实现 QStringList 属性 + 共用 NOTIFY 全量重建）。
    Q_PROPERTY(PlaylistModel* playlistModel READ playlistModel CONSTANT)

public:
    explicit PlayerViewModel(QObject *parent = nullptr);

    /// Register mpv property observers. Must run BEFORE startEvents.
    void attach(MpvController *mpv);

    /// 解除与 mpv 的绑定（mpv.destroy() 后调用，防 mpv_ 悬垂）。
    /// 事件线程已停止、无任何调用方后再调。
    void detach() { mpv_ = nullptr; }

    /// 事件线程：文件加载完成（MPV_EVENT_FILE_LOADED）后调用。
    /// 主线程同步 playing = !pause——每次加载完成重新对齐 UI 状态
    ///（曲目切换的 path 清空窗口会让状态短暂错位，此回调兜底收敛）。
    void onFileLoadedFromEventThread();

    /// Initialize VSR parameters from CLI strings
    /// ("off"/"auto"/"2"/"3"/"4", "low"/"medium"/"high"/"ultra").
    void initVsr(const std::string &scale, const std::string &quality,
                 const std::string &denoise);
    /// Initialize frame interpolation from CLI string. benchmark=true: value is
    /// a hard multiplier ("2"/"3"/"4", 0=off) — no passthrough, no persistence.
    /// Normal mode: "off" | "30" | "40" | "60" (target fps, persists fallback).
    void initFruc(const std::string &fruc, bool benchmark);

    void setGpuName(const QString &name);
    /// 持久化：启动时读（须在 initVsr 前调用——scale/quality/denoise
    /// 决定 vf 字符串）；各 setter 写入。benchmark 模式不调用（测量口径）。
    void loadSettings();
    /// mpv init 后应用持久化的音量/倍速/循环（观察器随后回填 viewModel）。
    void applyPlaybackSettings();
    /// 恢复上次播放列表（无 CLI 文件时调用）：首条 replace 开始播放，
    /// 其余 append，最后定位上次条目。
    void restorePlaylist();
    /// 轨道记忆（按文件，QSettings trackMem map：path →
    /// {aid,sid,vid,subs,sel,subDelay}；trackMemOrder 记录
    /// LRU 访问序，上限 10 个文件，超限淘汰最久未用）。
    /// 即时落盘：每个轨道设置动作（selectTrack/toggleSubtitles/
    /// adjustSubDelay/sub-add）完成后同步写盘——保存点乐观更新，记忆
    /// 永远反映最近一次用户操作。可见性无独立配置（是否显示由 sid
    /// 决定）。恢复 = 文件加载完成 hook（FILE_LOADED，打开文件与
    /// 播放列表选取同路径）：有记忆恢复，无记忆走自动字幕策略
    ///（autoSelectSubtitle，2026-08-06）。
    void saveTrackMemory();
    void restoreTrackMemory(const QString &path);
    void setFullscreen(bool fs);   // Q_PROPERTY WRITE（QML 窗口同步回写）

    bool playing() const        { return playing_; }
    int64_t currentTime() const { return currentTime_.load(); }
    int64_t duration() const    { return duration_.load(); }
    bool overlaysVisible() const { return overlaysVisible_; }
    double volume() const       { return volume_; }
    bool muted() const          { return volume_ <= 0.0; }
    int quality() const         { return quality_.load(); }
    bool vsrActive() const      { return scale_.load() != -1 || denoise_.load() != -1; }
    double scale() const        { return scale_.load(); }
    int denoiseQuality() const  { return denoise_.load(); }
    int frucFps() const         { return frucFps_.load(); }
    double speed() const        { return speed_.load(); }
    QString aspect() const      { return aspect_; }
    bool hwDecoding() const     { return hwDecoding_.load(); }
    QString videoInfo() const   { return videoInfo_; }
    int videoWidth() const { return videoWidth_.load(); }
    int videoHeight() const { return videoHeight_.load(); }
    QString videoCodec() const { return videoCodec_; }
    double videoFps() const { return videoFps_.load(); }
    int64_t decodedFrames() const { return decodedFrames_.load(); }
    int renderWidth() const { return renderWidth_.load(); }
    int renderHeight() const { return renderHeight_.load(); }
    int viewportWidth() const { return viewportWidth_.load(); }
    int viewportHeight() const { return viewportHeight_.load(); }
    QString decoderPixelFormat() const { return decoderPixelFormat_; }
    int64_t droppedFrames() const { return droppedCache_.load() - droppedBase_.load(); }
    int audioSampleRate() const { return audioSampleRate_.load(); }
    int audioChannels() const { return audioChannels_.load(); }
    double renderFps() const { return renderFps_.load(); }
    int64_t renderedFrames() const { return renderedFrames_.load(); }
    int videoBitDepth() const { return videoBitDepth_.load(); }
    bool fullscreen() const     { return fullscreen_; }
    bool osdVisible() const     { return osdVisible_; }
    QVariantList trackList() const { return trackList_; }
    bool subVisible() const { return subVisible_; }
    double subDelay() const { return subDelay_; }
    QVariantList subtitleFiles() const { return subtitleFiles_; }
    int loopMode() const        { return loopMode_; }
    PlaylistModel *playlistModel() { return &playlistModel_; }

    MpvController *mpv() const { return mpv_; }

    /// mpv `vf` 选项串（viewModel 单一来源，替代 Options::vfOption）。
    /// scale 为 OPT_FLOAT：auto→"0"、off→"-1"、4/3→"1.3333"（strtod 解析）。
    std::string vfOption() const;
    /// 线程安全的字符串化（只读 atomic，RPC 线程可调）：
    /// scale→"off"/"auto"/"4/3"/"1.5"/"2"/"3"/"4"；quality/denoise→枚举名。
    std::string scaleString() const;
    std::string qualityString() const;
    std::string denoiseString() const;
    /// 字符串→数值解析（RPC set-vsr 与 CLI initVsr 共用，DRY）。
    /// scale: "off"→-1, "auto"→0, "4/3"→4/3, "1.5"→1.5, "2"→2, "3"→3, "4"→4
    /// quality: "low"→1, "medium"→2, "high"→3, "ultra"→4
    /// denoise: "off"→-1, "low"→8, "medium"→9, "high"→10, "ultra"→11
    static bool parseScale(const std::string &s, double *out);
    static bool parseQuality(const std::string &s, int *out);
    static bool parseDenoise(const std::string &s, int *out);

public slots:
    /// 精确语义（与 mpv `set pause yes/no` 属性一致，非 toggle）：
    /// play：确保播放（idle → 重载 lastPath + un-pause）；pause：确保暂停。
    void play();
    void pause();
    void setPaused(bool p);
    void setMuted(bool m);
    /// 渲染新帧计数（Video uf>0 分支调用，主线程）——段内 rendered 统计。
    void notifyFrameRendered();
    void togglePlayPause();
    /// 单帧步进（调试插帧/闪回用）：dir>0 前进 1 帧（frame-step，
    /// 暂停态推进 1 帧后保持暂停）；dir<0 回退 1 帧（frame-back-step）。
    void frameStep(int dir);
    void stop();
    void seekAbsolute(int64_t ms);
    void seekRelative(int64_t offsetMs);
    void setVolume(double vol);
    void toggleMute();
    void toggleHwaccel();
    void setSpeed(double speed);
    Q_INVOKABLE void setAspect(const QString &v);
    void toggleFullscreen();
    void setOverlaysVisible(bool v);
    void toggleOsd();
    /// 直接打开 OSD（benchmark 模式无 UI，启动即显示；等价 toggleOsd 的开分支）。
    void showOsd();
    void screenshot();
    void setScale(double s);
    void setQuality(int q);
    void setDenoiseQuality(int d);
    void setFrucFps(int v);   // -1 | 30 | 40 | 60（乐观更新 + pushVf + saveSettings）
    // rife 状态行（事件线程从 mpv status 日志提取，"fruc-status:" 前缀）
    void setFrucStatus(const std::string &s);
    void toggleLoop();
    void setLoopMode(int m);   // 0=none, 1=file, 2=playlist（applyPlaybackSettings/toggleLoop 共用）
    void loadFile(const QString &path);
    /// 打开文件/追加/字幕（mode: 0=replace+queue, 1=append, 2=subs）。
    /// 路径分类（字幕扩展名 → sub-add）在 C++ 单一入口。
    void openFiles(const QStringList &paths, int mode);
    /// 轨道选择（type: "audio"/"video"/"sub"；sub 且 id<0 = 关闭字幕）
    void selectTrack(const QString &type, qlonglong id);
    void toggleSubtitles();          // sub-visibility 翻转
    void adjustSubDelay(double delta);  // 字幕延迟 ±0.1s 步进
    /// 重置字幕偏移为 0（"重置偏移"按钮；即时落盘按文件）
    Q_INVOKABLE void resetSubDelay();
    /// 加载外部字幕文件（sub-add select；复用 openFiles mode=2 分类）
    void loadExternalSubtitle(const QString &path) { openFiles({path}, 2); }
    /// 清除当前文件（lastPath_）的字幕记忆（sid/subs/sel/subDelay）——
    /// 下次打开该文件字幕回到默认状态；音轨/视频轨记忆保留。字幕项
    /// 全清时条目整体删除（trackMemOrder 同步移除）。
    /// 同时清除运行时字幕设置：全部外部字幕轨移除、字幕轨关闭
    ///（sid no）、偏移归零、**可见性关闭（清除设置 = 不显示字幕，
    /// 手动选字幕轨时恢复）**。
    Q_INVOKABLE void clearSubtitleMemory();
    void playlistRemove(int index);
    void playlistClear();
    void playlistNext();
    void playlistPrev();
    void playlistPlayIndex(int index);

signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void overlaysVisibleChanged();
    void volumeChanged();
    void qualityChanged();
    void vsrActiveChanged();
    void scaleChanged();
    void denoiseQualityChanged();
    void frucFpsChanged();
    void speedChanged();
    void aspectChanged();
    void hwDecodingChanged();
    void videoInfoChanged();
    void osdDataChanged();
    void fullscreenChanged();
    void osdVisibleChanged();
    void loopModeChanged();
    void trackListChanged();
    void subVisibleChanged();
    void subDelayChanged();
    void subtitleFilesChanged();

private:
    // ── 主线程状态更新（值由事件线程读好后随 lambda 传入；
    //    主线程绝不调 mpv client API —— untimed 下 flip_page 持
    //    core lock 等 render，主线程 get_property 会与其互锁导致
    //    渲染循环饿死（5fps 卡顿））──────────────────────────────
    void updatePlaying(bool p);
    void updateTime(int64_t t, int64_t d);
    void updateVolume(double v);
    void updateSpeed(double s);
    void updateHwDecoding(bool hw);
    void updateVideoInfo(const QString &f);
    void updateLoopMode(int m);
    /// idle 重播 lastPath：列表保留（stop keep-playlist）→ 查
    /// PlaylistModel 定位原条目，playlist-play-index 直接播放（列表
    /// 不变、不重复入列）；列表已无该路径 → append-play 追加。
    /// 不用 loadfile replace——cmd_loadfile 的 REPLACE 先 playlist_clear
    /// 清空整个列表（play()/togglePlayPause 共用，勿再引入 replace）。
    void resumeLastPath();
    /// 段内统计重置（停止/暂停翻转/seek 完成/新文件）。主线程调用。
    void resetSegmentCounters(int64_t dropBase);
    /// 事件线程：读 loop-file/loop-playlist 并回写 loopMode。
    void updateLoopModeFromEventThread();
    /// 扫描视频同目录字幕文件（主线程，文件 IO 低频）：排除已加载的
    /// 外部字幕（track-list external 轨），按优先级排序（精确同名 >
    /// 命名后缀 > 其余按文件名），条目带 prio 字段。
    void scanSubtitleFiles(const QString &videoPath);
    /// 无记忆加载时的字幕自动策略（restoreTrackMemory 无记忆分支）：
    /// 内置字幕轨按 locale 匹配 > 英文轨 > 目录匹配字幕文件
    ///（prio≤1）自动加载 > 其他不显示（sid no）。
    void autoSelectSubtitle();
    /// sub-add 即时落盘：pendingSubs_ 暂存 + 保存（mpv 异步回写
    /// trackList_ 前，保存须含刚加入的路径——否则丢失本次操作）。
    void noteExternalSubAdded(const QString &path);

    /// OSD 文本计算属性（事件线程 idle 回调拉取；纯本地读，零 mpv API）。
    /// 数据源 = viewModel 状态（观察器填充）——OSD 与 UI 同源。
    std::string osdTextString();
    /// OSD 拉取推送（事件线程，~100ms）：osdVisible 时拉取文本并推送
    /// osd-msg1/osd-level。事件线程调 mpv API 安全（主线程调会与
    /// untimed 渲染死锁，实测卡死）。
    void osdPoll();

    /// 热更新 VSR 参数：`vf-command @vsr <param> <value>`（async）。
    /// 不重建 filter 链——vf_vsr 的 vsr_command 更新私有选项，f_process
    /// 下一帧自动重配（scale→effective_scale 重算 / quality→ensure_vsr
    /// 检测 / denoise→passthrough 判定）。参数调用时同步复制，无需保活。
    void pushVf(const char *filter, const char *param, const std::string &value);
    std::string scaleStr() const;
    std::string qualityStr() const;
    std::string denoiseStr() const;
    /// 写持久化统一入口：benchmark 不读也不写（loadSettings 未调用 →
    /// settingsEnabled_ 恒 false，测量口径无副作用）。
    void saveSettings(const char *key, const QVariant &value);

    MpvController *mpv_ = nullptr;
    std::string hwdecInit_;      // 启动时 hwdec 值（切换硬解时恢复）
    bool playing_ = false;
    std::atomic<int64_t> currentTime_{0};
    std::atomic<int64_t> duration_{0};
    std::atomic<int> videoBitDepth_{0};   // 源视频色深（video-params 解析，0=未知）
    QString hwPixelFormat_;               // hw-pixelformat（硬解实际格式，如 p010）
    QString decoderName_;                 // 实际解码器（软解 libdav1d 等 / 硬解 codec 名）
    QString hwdecName_;                   // hwdec-current 原值（nvdec-copy/nvdec/no）
    bool overlaysVisible_ = true;
    double volume_ = 1.0;    // 初始与持久化默认一致（0.65 从未生效——观察器首轮回填覆盖）
    double savedVolume_ = 0.5;   // 静音前音量（toggle mute 恢复用；未静音过则 0.5）
    std::atomic<double> speed_{1.0};
    QString aspect_ = QStringLiteral("no");    // no=容器比例（mpv 0.41 -1 已弃用，no 等价无警告）
    std::atomic<bool> hwDecoding_{false};
    QString videoInfo_;
    // ── OSD 数据源（主线程观察器 post 写；事件线程 osdTextString 拉取读；
    //    数值成员原子化，QString 隐式共享线程安全）─────────────────
    std::atomic<int> videoWidth_{0}, videoHeight_{0};
    QString videoCodec_;
    std::atomic<double> videoFps_{0.0};
    std::atomic<int64_t> decodedFrames_{0};
    std::atomic<int> renderWidth_{0}, renderHeight_{0};
    std::atomic<int> viewportWidth_{0}, viewportHeight_{0};
    QString decoderPixelFormat_;
    std::atomic<int64_t> droppedBase_{0};      // frame-drop-count 段起始基准
    std::atomic<int64_t> droppedCache_{0};     // frame-drop-count 最新值（事件线程写）
    std::atomic<int> audioSampleRate_{0}, audioChannels_{0};
    std::atomic<double> renderFps_{0.0};
    std::atomic<int64_t> renderedFrames_{0};
    bool seekingPrev_ = false;       // seeking 观察器翻转检测
    bool pausePrev_ = false;         // pause 观察器翻转检测，初始通知 false→false 不触发
    // 渲染帧率差值基准：(renderedFrames_ 增量 / Δt)，osdTextString 计算用
    // （osdTextString 在事件线程调用——fpsTimer_/fpsPrevRendered_ 仅事件线程访问）
    int64_t fpsPrevRendered_ = 0;
    QElapsedTimer fpsTimer_;
    // ── OSD 状态（mpv 内部 osd-msg1 渲染）───────────────────────────
    // 事件线程 idle 回调拉取 osdTextString 并推送（benchmark 同方案）；
    // 主线程 toggleOsd 只翻转 osdVisible_/osdLevel_（atomic）。
    std::atomic<bool> osdVisible_{false};
    std::atomic<bool> osdLevel_{false};       // true=level 1（显示）
    bool osdLevelApplied_ = false;            // 事件线程专用：上次应用的 level
    std::string osdLastText_;                 // 事件线程专用：上次推送的文本
    bool fullscreen_ = false;
    std::atomic<int> quality_{3};
    std::atomic<int> denoise_{-1};
    std::atomic<double> scale_{0.0};
    std::atomic<int> frucFps_{-1};      // -1 off | 30/40/60 target (persisted)
    std::mutex frucStatusMtx_;
    std::string frucStatus_;            // 最新 rife 状态行（OSD 显示）
    std::atomic<int> frucScale_{0};     // benchmark multiplier 2/3/4 (CLI only)
    int loopMode_ = 0;           // 0 none, 1 loop-file, 2 loop-playlist
    PlaylistModel playlistModel_;   // 播放列表镜像（观察器 setSnapshot 增量喂入）
    QString gpuName_;

    // ── idle 重播（stop/正常播完 → 播放键从头重播）───────────────
    // 主线程状态，由事件线程的 "path" 属性观察回写：path 非空 →
    // 有活动文件；为空（停止/播完/未加载）→ fileLoaded_=false，
    // 播放按钮复位，togglePlayPause 检测后从 lastPath_ 重新加载。
    bool fileLoaded_ = false;
    QString lastPath_;

    // ── 音量节流（setVolume 合并投递）──────────────────────────────
    // 主线程 slot 高频调用（slider 拖动）只记录最新值；QueuedConnection
    // 每事件循环迭代最多投递一次（latest-wins，async 不阻塞）。无 QTimer。
    double pendingVolume_ = 0.0;
    bool volumeSetQueued_ = false;

    // ── 持久化（QSettings 原生格式 → ~/.config/vsr-player/vsr-player.conf）─
    // 主线程专用（setter/观察器 post 均主线程）。benchmark 不调用 loadSettings。
    QSettings settings_{QStringLiteral("vsr-player"), QStringLiteral("vsr-player")};
    bool settingsEnabled_ = false;   // loadSettings 调用后才允许写（benchmark 恒 false）
    QVariantList trackList_;      // track-list 观察器填充（Task 3）
    QStringList pendingSubs_;     // 刚 sub-add 未回写 trackList_ 的路径（保存合并用）
    bool subVisible_ = false;
    double subDelay_ = 0.0;
    QVariantList subtitleFiles_;  // 目录未加载外部字幕（{name,path}，优先级排序）
    double persistScale_ = 0.0;   // 持久化 VSR 参数（initVsr 用，CLI 未显式时生效）
    int persistQuality_ = 3;
    int persistDenoise_ = -1;
    int persistFruc_ = -1;        // 持久化插帧目标 fps（initFruc 用）
};
