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
#include "FrucBackend.h"   // 档位默认值/合法性（单一来源，见 fruc::kMaxFpsDefault）

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
    // 管线状态 OSD：2D 由 mpv 画进视频帧（osd-msg1）；3D 下 mpv 的 OSD 会被
    // 合成器按左/右半幅切开（文字被劈成两半）→ 3D 改用 QML 覆盖层渲染
    // osdText，mpv 侧强制 osd-level=0（见 osdPoll）。osdVisible 是同一个
    // 单一状态，两种渲染器共用。
    Q_PROPERTY(bool osdVisible READ osdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(QString osdText READ osdText NOTIFY osdTextChanged)
    // Audio
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    // 无独立 mute 状态位：volume == 0 即静音（派生属性，volumeChanged 通知）
    Q_PROPERTY(bool muted READ muted NOTIFY volumeChanged)
    // Video settings
    Q_PROPERTY(int quality READ quality NOTIFY qualityChanged)
    Q_PROPERTY(bool vsrActive READ vsrActive NOTIFY vsrActiveChanged)
    Q_PROPERTY(double scale READ scale NOTIFY scaleChanged)
    Q_PROPERTY(int denoiseQuality READ denoiseQuality NOTIFY denoiseQualityChanged)
    // Frame interpolation (FRUC): -1=off；>0 = 倍率（VFG 分支）/ 目标帧率
    // （RIFE 分支）——合法档位与标签见 FrucBackend.h（编译期分支）。
    Q_PROPERTY(int frucFps READ frucFps NOTIFY frucFpsChanged)
    // UI 档位模型（{label,value} 列表，随编译分支产出；QualityPopup 数据源）：
    // VFG = Off/×2/×3/×4，RIFE = Off/40 fps/48 fps/60 fps。编译期常量，CONSTANT。
    Q_PROPERTY(QVariantList frucModel READ frucModel CONSTANT)
    // 插帧质量档（仅 VFG：0=low / 1=medium / 2=high，见 FrucBackend.h）。
    // RIFE 分支该排隐藏（frucQualityAvailable=false），值恒为默认 medium。
    Q_PROPERTY(int frucQuality READ frucQuality NOTIFY frucQualityChanged)
    // 质量档 UI 模型（{label,value}，标签随编译分支；C++ 单一来源）。
    Q_PROPERTY(QVariantList frucQualityModel READ frucQualityModel CONSTANT)
    // 该排是否可见/可用：VFG true / RIFE false（编译期常量，CONSTANT）。
    Q_PROPERTY(bool frucQualityAvailable READ frucQualityAvailable CONSTANT)
    // 输出帧率上限（仅 VFG）：以当前倍率插帧后的帧率超过它就直通。
    // 48/60/120（见 FrucBackend.h kMaxFpsPresets；默认 120）。
    Q_PROPERTY(int frucMaxFps READ frucMaxFps NOTIFY frucMaxFpsChanged)
    Q_PROPERTY(QVariantList frucMaxFpsModel READ frucMaxFpsModel CONSTANT)
    Q_PROPERTY(bool frucMaxFpsAvailable READ frucMaxFpsAvailable CONSTANT)
    // ── 反交错（mpv `--deinterlace` 的三态开关）───────────────────────
    // 0=Off（mpv `no`，默认）、1=Auto（mpv `auto`，只处理解码器标记为隔行
    // 的帧）、2=Force（mpv `yes`，逐帧都处理）。实现完全交给 mpv 输出链的
    // `deint` 前置节点（filters/f_auto_filters.c）：该节点排在所有 `--vf`
    // 之前 → 我们的 hwup→vsr→truehdr→vfg 拿到的已经是逐行帧，不新增滤镜、
    // 不改链序。hwdec=nvdec（IMGFMT_CUDA）时用 bwdif_cuda（全程显存内）。
    // ⚠ send_field 语义 = 每个场一帧 → 帧率翻倍（25i→50p），下游 VSR/VFG
    // 按翻倍后的帧数工作（README/AGENTS 有实测）。
    Q_PROPERTY(int deintMode READ deintMode NOTIFY deintModeChanged)
    // ── 旋转（VFX 之前的 CUDA 零拷贝 90°/270°）─────────────────────────
    // 0=Off（默认）、90=顺时针、270=逆时针 90°（右键菜单三态，参考反交错）。
    // 由链上的 `@rot:rot=angle=N` 实现（hwup 之后、vsr/vfg 之前；NVRTC
    // kernel 直接读写显存平面，无下载/上传）。运行期 `vf-command rot angle N`
    // 即时生效、不重建链。90/270 交换宽高与像素宽高比；文件 rotate 元数据
    // 原样带出（菜单角度叠加在它之上，与 --video-rotate 语义一致）。
    Q_PROPERTY(int rotMode READ rotMode NOTIFY rotModeChanged)
    // ── SDR→HDR10 上变换（VFX TrueHDR，链上位于 vsr 之后；参数/证据见
    //    docs/vfx-truehdr-investigation.md §8 与 src/client/TrueHdrParams.h）──
    // 总开关 = HDR 策略的 opt-in（默认取 `VSR_UPHDR` env；UI 改后以持久化值为准）。
    Q_PROPERTY(bool upHdrEnabled READ upHdrEnabled NOTIFY upHdrEnabledChanged)
    // 策略**实际**是否在跑上变换（= Hdr10 上屏 ∧ 内容 SDR ∧ 总开关；过滤器
    // 还有一层按帧 transfer 的自守卫）——UI 只读展示。
    Q_PROPERTY(bool upHdrEffective READ upHdrEffective NOTIFY upHdrEffectiveChanged)
    // 面板自动峰值（合成器 preferred maxLuma，策略下发；只读，UI 显示"自动 Nnit"）
    Q_PROPERTY(int upHdrAutoLuminance READ upHdrAutoLuminance NOTIFY upHdrAutoLuminanceChanged)
    // Luminance 覆盖开关：关 = 自动跟面板（默认）；开 = 用 upHdrLuminance
    Q_PROPERTY(bool upHdrLuminanceOverride READ upHdrLuminanceOverride
               NOTIFY upHdrLuminanceOverrideChanged)
    Q_PROPERTY(int upHdrLuminance READ upHdrLuminance NOTIFY upHdrLuminanceChanged)
    Q_PROPERTY(int upHdrSaturation READ upHdrSaturation NOTIFY upHdrSaturationChanged)
    Q_PROPERTY(int upHdrContrast READ upHdrContrast NOTIFY upHdrContrastChanged)
    Q_PROPERTY(int upHdrMiddleGray READ upHdrMiddleGray NOTIFY upHdrMiddleGrayChanged)
    // 抗色带（true = 开 = SDK DebandingOff 0；默认开）
    Q_PROPERTY(bool upHdrDebanding READ upHdrDebanding NOTIFY upHdrDebandingChanged)
    // UI 用的区间/默认值（单一来源 TrueHdrParams.h，reset 目标取这里的 *Def）
    Q_PROPERTY(QVariantMap upHdrLimits READ upHdrLimits CONSTANT)
    // Speed
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    // Aspect ratio（mpv video-aspect-override：-1=auto, no=不覆盖, 或 16:9 等）
    Q_PROPERTY(QString aspect READ aspect NOTIFY aspectChanged)
    // 单眼 2D 输出（0=off / 1=左眼 / 2=右眼）；运行期状态，不持久化。
    Q_PROPERTY(int eyeMode READ eyeMode NOTIFY eyeModeChanged)
    /// 片源格式自动判定（-1 未判定 / 0 左右 / 1 上下；目前只判"全幅"）。
    Q_PROPERTY(int sourceLayoutHint READ sourceLayoutHint NOTIFY sourceLayoutHintChanged)
    /// 单眼输出的眼别偏好（0=左 1=右）。与 eyeMode 分开：关闭状态下菜单也能
    /// 选"左眼/右眼"，下次进单眼模式直接用这个偏好。
    Q_PROPERTY(int eyePref READ eyePref NOTIFY eyePrefChanged)
    // Info（解码器状态分三个概念，别混用）：
    //  · hwDecoding        = **实际**是否在硬解（= mpv `hwdec-current` != no）
    //  · hwDecodeEnabled   = **用户偏好**（我们下发给 mpv 的 hwdec 开关；
    //                        当前文件能否硬解**不影响**它——软解回退不得篡改）
    //  · hwDecodeAvailable = 当前文件是否**能**硬解（偏好=硬解却拿到软解 →
    //                        false；UI 据此显示"软解"并禁用切换按钮）
    Q_PROPERTY(bool hwDecoding READ hwDecoding NOTIFY hwDecodingChanged)
    Q_PROPERTY(bool hwDecodeEnabled READ hwDecodeEnabled NOTIFY hwDecodeEnabledChanged)
    Q_PROPERTY(bool hwDecodeAvailable READ hwDecodeAvailable NOTIFY hwDecodeAvailableChanged)
    // 不能硬解时的**原因**（0=可切换、1=设备层：没有可用的 CUDA、2=文件层：
    // codec 不支持硬解）。区分这两者很重要：设备层时把原因写成"该文件只能
    // 软解"会误导（h264 明明能硬解，是这台设备解不了）。
    Q_PROPERTY(int hwDecodeReason READ hwDecodeReason NOTIFY hwDecodeAvailableChanged)
    // 当前 Vulkan 设备能否跑 AI 处理（超分/插帧/SDR→HDR10 上变换）：要求存在
    // **UUID 相同的 CUDA 设备**——我们的链是 CUDA 产出 + Vulkan 零拷贝导入，
    // 两张卡的组合只会黑屏。false 时整个 vf 链不插入（直通），AI 相关 UI 禁用。
    Q_PROPERTY(bool aiAvailable READ aiAvailable NOTIFY aiAvailableChanged)
    /// 当前播放条目能否"删除文件"（只允许本地常规可写文件；URL/流/只读一律
    /// false → 右键菜单里该项隐藏）。
    Q_PROPERTY(bool canDeleteCurrentFile READ canDeleteCurrentFile NOTIFY canDeleteCurrentFileChanged)
    /// 是否已载入媒体（右键菜单"移除/删除当前条目"只在有当前条目时显示）。
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)
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
    // 内容是否 HDR（video-params/gamma ∈ {pq, hlg}）——HDR 上屏的内容侧门控。
    Q_PROPERTY(bool videoHdr READ videoHdr NOTIFY videoHdrChanged)
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

    // ── TrueHDR 参数（getter；setter 见 public slots）──────────────────
    bool upHdrEnabled() const { return upHdrEnabled_; }
    bool upHdrEffective() const { return upconvertOn_; }
    int  upHdrAutoLuminance() const { return (int)lroundf(upHdrAutoNits_); }
    bool upHdrLuminanceOverride() const { return upHdrLumOverride_; }
    int  upHdrLuminance() const { return upHdrLuminance_; }
    int  upHdrSaturation() const { return upHdrSaturation_; }
    int  upHdrContrast() const { return upHdrContrast_; }
    int  upHdrMiddleGray() const { return upHdrMiddleGray_; }
    bool upHdrDebanding() const { return upHdrDebanding_; }
    QVariantMap upHdrLimits() const;

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

    /// 事件线程：MPV_EVENT_PLAYBACK_RESTART（一次 seek / 加载完成、目标帧已
    /// 显示）后调用。连续快进/快退门控的完成信号——**必须用这个事件，不能用
    /// `seeking` 属性观察**：libmpv 对观察属性做**值去重**（`send_client_
    /// property_changes` 只在值与上次下发的不同时才发），连续 seek 时
    /// true/false 相互抵消，最后一次完成通知可能根本不会送达（实测：拖进度条
    /// 后方向键永久卡住）。事件流不去重，逐次到达。
    void notePlaybackRestart();

    /// Initialize VSR parameters from CLI strings
    /// ("off"/"auto"/"2"/"3"/"4", "low"/"medium"/"high"/"ultra").
    void initVsr(const std::string &scale, const std::string &quality,
                 const std::string &denoise);
    /// Initialize frame interpolation from CLI string. benchmark=true: value is
    /// a hard multiplier ("2"/"3"/"4", 0=off) — no passthrough, no persistence.
    /// 编译期分支（见 FrucBackend.h）：VFG 分支 --fruc 为倍率语义
    /// "off"|"2"|"3"|"4"；RIFE 分支保持既有 "off"|"40"|"48"|"60"（目标帧率）
    /// 与 "2"/"3"|"4"（benchmark 倍率）。
    void initFruc(const std::string &fruc, bool benchmark);
    /// vf 链顺序（CLI --fruc-order）：VFG 分支 "pre"|"post"（默认 post，
    /// 后置 hwup → vsr → vfg）；RIFE 分支链序固定，仅记日志提示忽略。
    void initFrucOrder(const std::string &order);
    /// 插帧质量档（CLI --fruc-quality）：VFG 分支 "low"|"medium"|"high"
    /// （默认 medium，见 FrucBackend.h）；RIFE 分支无此档，仅记日志提示忽略
    /// （照 initFrucOrder）。CLI 未显式指定（空）→ 用持久化值。
    void initFrucQuality(const std::string &quality);
    /// 输出帧率上限（CLI --fruc-max-fps）：VFG 分支 "48"|"60"|"120"
    /// （默认 120）；RIFE 分支无此概念，仅记日志提示忽略。空 → 持久化值。
    void initFrucMaxFps(const std::string &max_fps);

    /// 反交错初始值（CLI `-- --deinterlace=no|auto|yes` 优先于持久化值；
    /// cliMode < 0 = 未指定 → 用 QSettings 的 deintMode）。须在
    /// applyPlaybackSettings 之前调用。
    void initDeinterlace(int cliMode);
    /// 把当前模式下发给 mpv（`set options/deinterlace …`）。运行期切换由
    /// deint 节点逐帧读选项生效，不重建滤镜链（VSR 引擎保留）。
    void applyDeinterlace();
    /// "off"/"no"→0、"auto"→1、"force"/"yes"→2，非法→-1（RPC/CLI 共用）。
    static int parseDeintMode(const std::string &s);

    /// AI 可用性（启动时由 VulkanContext UUID × CUDA 设备判一次；见 CudaProbe）。
    /// available=false → 日志一条 WARN，`vfOption()` 返回空（不插任何滤镜），
    /// 所有 AI 设置入口拒绝生效（**不改用户持久化的设置**）。
    /// uuidHex = 选中的 Vulkan 设备 UUID（32 hex，无分隔符），会透给
    /// `@hwup:hwup:cuda-uuid=`，让滤镜按 UUID 选 CUDA 设备（多卡机器必需）。
    void initAiAvailability(bool available, const std::string &reason,
                            const std::string &uuidHex = std::string());

    void setGpuName(const QString &name);
    /// 持久化：启动时读（须在 initVsr 前调用——scale/quality/denoise
    /// 决定 vf 字符串）；各 setter 写入。benchmark 模式不调用（测量口径）。
    void loadSettings();
    /// mpv init 后应用持久化的音量/倍速/循环（观察器随后回填 viewModel）。
    void applyPlaybackSettings();
    /// 恢复上次播放列表（无 CLI 文件时调用）：首条 replace 开始播放，
    /// 其余 append，最后定位上次条目。
    void restorePlaylist();
    /// 启动恢复 bd:// 条目前设回 bluray-device（见 .cpp 注释）。
    void restoreBlurayDevice(const QStringList &files);
    /// 只恢复播放列表、**不载入任何媒体**（启动 idle，见 main.cpp）：
    /// 全部条目 append（无 current 条目 → mpv 不建解码/滤镜链、不 Load
    /// 引擎、不做续播 seek），仅把列表填进 UI 并记住"播放键该载入哪一条"。
    /// 用户按播放 → play()/togglePlayPause() → resumeLastPath() →
    /// playlist-play-index：此时才载入，mpv 按 watch_later 自动恢复到上次
    /// 进度（与从列表正常启动完全同一条路径）。
    void restorePlaylistIdle();
    /// 无参数启动将播放的条目（= settings playlist[playlistCurrent]，
    /// 越界回退首条；无列表则空）。与 restorePlaylist 同一来源——供
    /// main.cpp 在 mpv 初始化前判定"是否以 idle 态启动"。
    QString restoredCurrentFile() const;
    /// 该路径是否存在历史播放状态（mpv watch_later 条目存在）。
    /// 条目名 = MD5(路径) 大写十六进制（对照 third_party/mpv
    /// player/configfiles.c 的 mp_get_playback_resume_config_filename +
    /// misc/hash.c 的 "%02X" 输出），位于 MpvController::watchLaterDir()。
    /// 路径必须与交给 mpv 的字符串逐字节一致（在 loadfile 前用同一个
    /// QString 调用）——mpv 不做本地路径规范化。
    /// 条目必须含 `start=` 才算"有可恢复位置"——mpv 对不可 seek 的源只写
    /// 音量/轨道等（无 start），此时不该判为有历史。
    static bool hasResumeState(const QString &path);
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
    /// mpv 报告 seek 失败（"Cannot seek ..."）→ 事件线程调用，转主线程标记直播。
    void noteLiveSeekFailure();

    bool playing() const        { return playing_; }
    /// 有活动文件（未加载/停止/播完 → false）。playing_ 在文件装载前可能
    /// 因 mpv pause 属性初值短暂为 true——"是否真的在播"需与本标志合取。
    bool hasFile() const        { return fileLoaded_; }
    /// 当前文件是直播/线性流（不可 seek 或仅部分可 seek）。判定来源：加载后
    /// 的 seekable/partially-seekable 属性，或 mpv 的 "Cannot seek..." 报错
    ///（HLS 直播的滑动窗口：属性说可 seek，但上次保存的位置已滑出窗口）。
    /// 命中即：撤销本次自动暂停（跟最新）、删掉该文件 watch_later 条目
    ///（否则每次启动都重演 seek 风暴）、退出时不写位置。
    bool isLiveSource() const;
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
    /// UI 档位模型（编译分支决定 label/value，见 FrucBackend.h）。
    QVariantList frucModel() const;
    /// 插帧质量档（仅 VFG 有效；RIFE 分支恒默认 medium）。
    int frucQuality() const     { return frucQuality_.load(); }
    /// 质量档 UI 模型 / 该排是否可用（编译分支决定，见 FrucBackend.h）。
    QVariantList frucQualityModel() const;
    bool frucQualityAvailable() const;
    /// 输出帧率上限（仅 VFG；RIFE 分支恒默认值且该排隐藏）。
    int frucMaxFps() const      { return frucMaxFps_.load(); }
    int deintMode() const       { return deintMode_.load(); }
    int rotMode() const         { return rotMode_.load(); }
    QVariantList frucMaxFpsModel() const;
    bool frucMaxFpsAvailable() const;
    double speed() const        { return speed_.load(); }
    QString aspect() const      { return aspect_; }
    int eyeMode() const         { return eyeMode_.load(); }
    int sourceLayoutHint() const { return sourceLayoutHint_.load(); }
    int eyePref() const         { return eyePref_.load(); }
    bool hwDecoding() const     { return hwDecoding_.load(); }
    bool hwDecodeEnabled() const   { return hwDecodeEnabled_.load(); }
    bool hwDecodeAvailable() const { return hwDecodeAvailable_.load(); }
    int hwDecodeReason() const { return hwDecodeReason_.load(); }
    bool aiAvailable() const { return aiAvailable_.load(); }
    bool canDeleteCurrentFile() const { return canDeleteCurrentFile_; }
    bool hasMedia() const { return fileLoaded_; }
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
    bool videoHdr() const { return videoHdr_; }

    /// 内容 HDR10 静态元数据（mpv `video-params/{min-luma,max-luma,max-cll,max-fall}`，
    /// 单位 nit）。用途：HdrOutput 把它下发给合成器当"内容峰值"（见 hdr::ContentMeta）。
    struct HdrMeta {
        bool   valid = false;    ///< false = 源无静态元数据
        double minMastering = 0; ///< ST 2086 mastering min L
        double maxMastering = 0; ///< ST 2086 mastering max L
        double maxCll = 0;       ///< CTA-861 MaxCLL（0 = 未定义）
        double maxFall = 0;      ///< CTA-861 MaxFALL（0 = 未定义）
    };
    HdrMeta hdrMeta() const { return hdrMeta_; }
    int64_t droppedFrames() const { return droppedCache_.load() - droppedBase_.load(); }
    int audioSampleRate() const { return audioSampleRate_.load(); }
    int audioChannels() const { return audioChannels_.load(); }
    double renderFps() const { return renderFps_.load(); }
    int64_t renderedFrames() const { return renderedFrames_.load(); }
    int videoBitDepth() const { return videoBitDepth_.load(); }
    bool fullscreen() const     { return fullscreen_; }
    bool osdVisible() const     { return osdVisible_; }
    QString osdText() const;   // 最近一次算出的 OSD 文本（互斥量保护）
    /// 3D 是否激活：决定 OSD 走 mpv（2D）还是 QML 覆盖层（3D）。
    void setStereo3DActive(bool v) { stereo3d_.store(v); }
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

    /// 3D 每眼映射（vf_vsr 的 auto 倍率按"眼视口 vs 眼源像素"算）。
    /// active=false → 回 2D 公式。链重建后由 syncVfOptions 重放。
    void setStereoMap(bool active, int layout, int half);
    /// 单眼 2D 输出（vf_eye）：mode 0=off/1=left/2=right；layout 0=SBS/1=TAB；half 0/1。
    /// 与 setStereoMap 分开：3D 关闭时 vf_vsr 走 2D 公式（stereoLayout_=-1），
    /// 而 vf_eye 始终需要真实布局/尺寸。
    void setEyeState(int mode, int layout, int half);
    /// 只设置眼别偏好（0=左 1=右）；若单眼模式正在跑，同时切换显示的眼。
    void setEyePref(int pref);
    /// 链重建后重放 3D 每眼映射（事件线程调用 → 内部 async 命令）。
    /// mpv 在解码器软硬切换/格式变化时会用**加载时的 vf 选项串**重建用户
    /// 滤镜链，运行期 vf-command 设的值随之丢失（2026-10-26 实测：3D 倍率
    /// 在软硬解码切换后回到 2D 公式）——所以 VIDEO_RECONFIG 时补推一次。
    void replayStereoMap();

    /// 快捷键调音量（↑/↓）。与滑杆的 setVolume 区分：只有本方法发
    /// volumeNudged()——2D 下音量提示只对"按键"出现，UI 拖动不弹提示。
    Q_INVOKABLE void nudgeVolume(double delta);
    void setVolume(double vol);
    void toggleMute();
    void toggleHwaccel();
    void setSpeed(double speed);
    Q_INVOKABLE void setAspect(const QString &v);
    /// 触发一次瞬态提示（3D 与 2D 都会显示）。
    Q_INVOKABLE void flashMessage(const QString &value) { emit messageFlashed(value); }
    /// 播放列表里实际显示的条目名（面板 = PlaylistModel.display）。
    Q_INVOKABLE QStringList playlistDisplayNames() const
    { return playlistModel_.displayNames(); }
    void toggleFullscreen();
    void setOverlaysVisible(bool v);
    void toggleOsd();
    /// 直接打开 OSD（benchmark 模式无 UI，启动即显示；等价 toggleOsd 的开分支）。
    void showOsd();
    void screenshot();
    void setScale(double s);
    void setQuality(int q);
    void setDenoiseQuality(int d);
    void setFrucFps(int v);   // 档位（见 FrucBackend.h：VFG -1|2|3|4；RIFE -1|40|48|60）
    /// 插帧质量档（仅 VFG：0|1|2 = low|medium|high）。热切换 vf-command
    /// `vfg mode <name>`（filter 下帧重配引擎，不重建链）+ 持久化。
    void setFrucQuality(int v);
    /// 输出帧率上限（仅 VFG：48|60|120）。vf-command `vfg max-fps <n>` +
    /// 持久化；filter 下一帧重估直通（超过上限 → reason=max-fps）。
    void setFrucMaxFps(int v);
    /// 反交错三态（0/1/2，见 deintMode）：下发 mpv + 持久化。
    Q_INVOKABLE void setDeintMode(int mode);
    /// 旋转三态（0/90/270，见 rotMode）：vf-command 下发 + 持久化。
    Q_INVOKABLE void setRotMode(int deg);
    /// FRUC 状态行（事件线程从 mpv status 日志提取；前缀随后端：
    /// VFG "vfg-status:" / RIFE "fruc-status:"，见 FrucBackend.h）
    void setFrucStatus(const std::string &s);
    void setVsrStatus(const std::string &s);
    /// HDR 上屏状态行（`hdr::statusText()`，OSD 显示）。
    /// 由 GUI 线程在 hdr 策略钩子里设置；osdTextString 在事件线程读 → 加锁。
    void setHdrStatus(const std::string &s);
    /// **实际输出**模式（`hdr::outputText()` = HDR10/SDR，OSD 的"输出"行用）。
    /// 同 `setHdrStatus`：GUI 线程写、事件线程读 → 加锁。
    void setHdrOutput(const std::string &s);
    /// SDR→HDR10 上变换（VFX TrueHDR，链上位于 vsr 之后）的开关与峰值 —— 由
    /// HDR 策略钩子（hdr::ApplyHooks.updateUpconvert）在 GUI 线程调用。
    ///   autoNit      = 面板物理峰值（合成器 preferred maxLuma；UI 显示"自动 Nnit"）
    ///   effectiveNit = 实际生效峰值（覆盖开关打开时是用户值，否则 = autoNit）
    /// 内部记住状态（FILE_LOADED 重放用）＋ 下发 `vf-command truehdr enable|luminance`。
    /// 两个值都夹到 SDK 合法区间 [400,2000]。
    void setUpconvert(bool on, float autoNit, float effectiveNit);
    /// TrueHDR 参数的**用户入口**（UI/RPC 调，GUI 线程）：夹取 → 存 → 持久化 →
    /// 下发 `vf-command truehdr <param>`。`luminance` 只在覆盖开关打开时生效
    ///（关 = 自动跟面板，见 setUpconvert 的 peakNit）。幂等（值未变只存不重发）。
    void setUpHdrEnabled(bool on);
    void setUpHdrLuminanceOverride(bool on);
    void setUpHdrLuminance(int nits);
    void setUpHdrSaturation(int v);
    void setUpHdrContrast(int v);
    void setUpHdrMiddleGray(int v);
    void setUpHdrDebanding(bool on);
    /// 持久化 TrueHDR 参数 → 成员 + 通知策略（main.cpp 在 loadSettings 后调；
    /// benchmark 不读设置 → 只按 env 默认）。
    void initTrueHdr();
    void toggleLoop();
    void setLoopMode(int m);   // 0=none, 1=file, 2=playlist（applyPlaybackSettings/toggleLoop 共用）
    void loadFile(const QString &path);
    /// 打开文件/追加/字幕（mode: 0=replace+queue, 1=append, 2=subs）。
    /// 路径分类（字幕扩展名 → sub-add）在 C++ 单一入口。
    /// 目录路径不扫描，原样交给 mpv `loadfile`（mpv 原生目录展开：
    /// 顶层媒体展开成条目、子目录留作可点击展开的条目）——打开文件夹
    /// 与拖入文件夹因此走同一条通路（main.qml FolderDialog 用 mode 0）。
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
    /// 从播放列表移除**当前**条目（Delete）：只动列表，不碰磁盘。
    Q_INVOKABLE void removeCurrentFromPlaylist();
    /// 删除**当前文件**（Shift+Delete）：移入回收站，再按上面的语义前进。
    Q_INVOKABLE void deleteCurrentFile();

signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void overlaysVisibleChanged();
    /// 直播/线性流被识别（主线程发出）：main.cpp 据此撤销"因历史进度而暂停"。
    void liveSourceDetected();
    void volumeChanged();
    /// 快捷键调了音量（UI 滑杆/API 不发）——音量提示 OSD 的触发源。
    void volumeNudged();
    /// 播放列表由非空变为空（清空/删完）→ 画面置黑。
    void playlistEmptied();
    /// 快捷键快进/快退（拖进度条走 seekAbsolute，不发）——进度提示 OSD 的触发源。
    void seekNudged();
    void qualityChanged();
    void vsrActiveChanged();
    void scaleChanged();
    void denoiseQualityChanged();
    void frucFpsChanged();
    void frucQualityChanged();
    void frucMaxFpsChanged();
    void deintModeChanged();
    void rotModeChanged();
    // TrueHDR（SDR→HDR10 上变换）参数
    void upHdrEnabledChanged();
    void upHdrEffectiveChanged();
    void upHdrAutoLuminanceChanged();
    void upHdrLuminanceOverrideChanged();
    void upHdrLuminanceChanged();
    void upHdrSaturationChanged();
    void upHdrContrastChanged();
    void upHdrMiddleGrayChanged();
    void upHdrDebandingChanged();
    void speedChanged();
    void aspectChanged();
    /// 瞬态提示（键位操作反馈：比例/布局等）。文案由 QML 组合（qsTr），
    /// 这里只传值。"no" = 不覆盖。
    void messageFlashed(const QString &value);
    /// 片源格式自动判定结果的变化通知（-1 = 无结论 / 0 = 左右 / 1 = 上下）。
    /// 用**粘性属性**而不是一次性信号：判定可能发生在 QML 就绪之前
    ///（benchmark/命令行直接载入），QML 在 completed 时按 sourceLayoutHint 补应用。
    void sourceLayoutHintChanged();
    void eyePrefChanged();
    void eyeModeChanged();
    void hwDecodingChanged();
    void hwDecodeEnabledChanged();
    void hwDecodeAvailableChanged();
    void aiAvailableChanged();
    void canDeleteCurrentFileChanged();
    void hasMediaChanged();
    void videoInfoChanged();
    void osdDataChanged();
    void videoHdrChanged();
    void videoHdrMetaChanged();   // 内容静态元数据变化（HDR10 描述需重下发）
    void fullscreenChanged();
    void osdVisibleChanged();
    /// OSD 文本变化（事件线程发出；QML 在 GUI 线程接收 → 自动排队）
    void osdTextChanged();
    void loopModeChanged();
    void trackListChanged();
    void subVisibleChanged();
    void subDelayChanged();
    void subtitleFilesChanged();

private:
    // ── 主线程状态更新（值由事件线程读好后随 lambda 传入；
    //    主线程绝不调 mpv client API —— untimed 下主线程同步调
    //    client API 持 client lock 等 core lock，播放循环（持 core
    //    lock）经 vo_thread flip_page 等主线程渲染 → 互锁死锁
    //    （实测卡死））────────────────────────────────────────────
    void updatePlaying(bool p);
    void updateTime(int64_t t, int64_t d);
    void updateVolume(double v);
    void updateSpeed(double s);
    void updateHwDecoding(bool hw);
    /// 由 hwdec-current / 解码器 / video-params 观察器与 FILE_LOADED 调用：
    /// 刷新"当前文件能否硬解"（偏好=硬解 ∧ 解码器已就绪 ∧ 实际软解 → 不可）。
    void refreshHwDecodeAvailability(int forceReason = 0);
    void updateVideoInfo(const QString &f);
    void updateLoopMode(int m);
    /// idle 重播 lastPath：列表保留（stop keep-playlist）→ 查
    /// PlaylistModel 定位原条目，playlist-play-index 直接播放（列表
    /// 不变、不重复入列）；列表已无该路径 → append-play 追加。
    /// 不用 loadfile replace——cmd_loadfile 的 REPLACE 先 playlist_clear
    /// 清空整个列表（play()/togglePlayPause 共用，勿再引入 replace）。
    void resumeLastPath();
    /// 恢复列表的持久化来源 + 合法化当前索引（越界/缺失 → 0）：
    /// restoredCurrentFile()（启动暂停判定）与 restorePlaylist()（实际装载）
    /// 共用——避免"哪一条会被播放"的规则分叉。
    QStringList restoredFiles(int *cur) const;
    /// 上次播放列表的快照文件（m3u8，带 #EXTINF 标题）。
    QString playlistSnapshotPath() const;
    void writePlaylistSnapshot(const QStringList &files,
                               const QStringList &titles, int cur) const;
    /// 段内统计重置（停止/暂停翻转/seek 完成/新文件）。主线程调用。
    void resetSegmentCounters(int64_t dropBase);
    /// 标记当前文件为直播/线性流（幂等）；why 仅用于日志。
    void markLiveSource(const char *why);
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

    bool upconvertOn() const { return upconvertOn_; }
    /// FRUC 滤镜名（编译期分支的单一出口：vf 串 label、vf-command 目标）。
    /// 定义见 FrucBackend.h（VFG "vfg" / RIFE "rife"）。
    const char *frucFilterName() const;
    /// vf 链顺序是否前置插帧（仅 VFG 分支可能为 true；RIFE 恒 false）。
    bool frucOrderPre() const { return frucOrderPre_.load(); }
    /// 运行时 vf 参数重放（FILE_LOADED 后，主线程）：vf-command 只改
    /// filter 实例 opts，mpv 的 vf 选项本体不变——stop（terminate_playback
    /// 销毁链）后重播重建 filter 时状态回到 vfOption 初始值 → UI 修改
    /// 丢失（插帧"关不掉"：关闭后重播自动恢复开启）。FILE_LOADED 时
    /// 链已就绪，重放当前 UI 状态使新链与 UI 一致（filter 常驻 +
    /// 直通切换语义不变，不重建链；判等 no-op 防冗余）。
    void syncVfOptions();
    /// 当前**生效**的 TrueHDR 目标峰值：覆盖开关打开 → 用户值；否则面板自动值
    ///（读取不到时退 SDK 默认 650）。
    int effectiveUpHdrLuminance() const;
    /// 把 5 个 TrueHDR 参数全部下发（`vf-command truehdr …`；enable 另经策略钩子）。
    /// 参数变化、FILE_LOADED 重放（链重建后回到链串默认值）都经此。
    void pushTrueHdrParams();
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
    void applyAspect();                          // 按 2D/3D 语义下发比例
    void guessSourceFormat(int w, int h);        // 按原生像素尺寸猜片源格式
    int sourceFormatLayout_ = -1;                // 上次判定结果（-1 = 未判定）
    int sourceFormatHalf_ = 0;
    std::atomic<int> sourceLayoutHint_{-1};      // QML 可见的判定结果
    std::atomic<int> eyePref_{0};                // 单眼偏好：0=左 1=右
    QString aspectApplied_ = QStringLiteral("no");  // 上次下发给 mpv 的值
    QString aspect_ = QStringLiteral("no");    // no=容器比例（mpv 0.41 -1 已弃用，no 等价无警告）
    std::atomic<bool> hwDecoding_{false};      // 实际（hwdec-current != no）
    std::atomic<bool> hwDecodeEnabled_{true};  // 用户偏好（我们下发的 hwdec 开关）
    std::atomic<bool> hwDecodeAvailable_{true};// 当前文件能否硬解（UI 禁用依据）
    std::atomic<int> hwDecodeReason_{0};       // 0=可切换 1=设备无 CUDA 2=codec 限制
    std::atomic<bool> aiAvailable_{true};      // 当前 Vulkan 设备能否跑 AI（启动时判定）
    std::string aiUnavailableReason_;          // 诊断串（日志/OSD 用）
    std::string aiCudaUuid_;                   // 选中 Vulkan 设备的 UUID（32 hex）
    /// AI 相关设置入口的统一守卫：不可用时**拒绝**（返回 false）并记一次 WARN。
    bool aiAllowed(const char *what);
    /// 重算 canDeleteCurrentFile_（path 变化时调用）。
    /// ⚠ path 必须由调用方传入（path 观察器手里已经有值）。**不要**在这里
    /// 重读 mpv 属性：本函数在 GUI 线程执行，而软解 + vo=libmpv 的 DR 帧
    /// 分配要靠 GUI 线程在 mpv_render_context_update() 里服务，同步
    /// mpv_get_property 会把整条渲染/解码链锁死（2026-10-24 gdb 实证）。
    void refreshCanDeleteCurrentFile(const QString &path);
    bool canDeleteCurrentFile_ = false;
    QString deletePath_;      // 与 canDeleteCurrentFile_ 同源（GUI 线程缓存）
    int playlistPos_ = -1;    // 缓存的 playlist-pos（playlist 观察器 post 写）
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
    bool videoHdr_ = false;   // video-params/gamma = pq|hlg（主线程持有）
    HdrMeta hdrMeta_;         // video-params 的 HDR10 静态元数据（主线程持有）
    // SDR→HDR10 上变换（VFX TrueHDR）状态：
    //  · upconvertOn_ = HDR 策略**实际**判定（Hdr10 ∧ 内容 SDR ∧ 总开关）→ enable
    //  · upHdrAutoNits_ = 面板物理峰值（策略经 setUpconvert 传入，运行期量，不持久化）
    //  · 其余 = 用户在 UI 里设定的参数（持久化；syncVfOptions/vfOption 重放）
    bool  upconvertOn_ = false;
    bool  upHdrEnabled_ = false;
    bool  upHdrLumOverride_ = false;
    int   upHdrLuminance_ = 650;
    int   upHdrSaturation_ = 100;
    int   upHdrContrast_ = 100;
    int   upHdrMiddleGray_ = 50;
    bool  upHdrDebanding_ = true;      // true = 抗色带开（SDK DebandingOff=0）
    float upHdrAutoNits_ = 0.0f;       // 面板峰值（显示用）
    float upHdrEffectiveNits_ = 650.0f; // 实际生效峰值（策略算好传进来）
    // 持久化中转（loadSettings 读、initTrueHdr 用；benchmark 下不读设置）
    bool  persistUpHdrEnabled_ = false;
    bool  persistUpHdrLumOverride_ = false;
    int   persistUpHdrLuminance_ = 650;
    int   persistUpHdrSaturation_ = 100;
    int   persistUpHdrContrast_ = 100;
    int   persistUpHdrMiddleGray_ = 50;
    bool  persistUpHdrDebanding_ = true;
    std::atomic<int64_t> droppedBase_{0};      // frame-drop-count 段起始基准
    std::atomic<int64_t> droppedCache_{0};     // frame-drop-count 最新值（事件线程写）
    std::atomic<int> audioSampleRate_{0}, audioChannels_{0};
    std::atomic<double> renderFps_{0.0};
    std::atomic<int64_t> renderedFrames_{0};
    bool seekingPrev_ = false;       // seeking 观察器翻转检测
    // 连续快进/快退门控（2026-10-21）：mpv 在一次 seek 尚未出帧时会合并/丢弃
    // 后续 seek，按住方向键时表现为整段跳画面。改为：上一次 seek 明确成功
    // （mpv restart complete，帧已显示）之后才触发下一次；在途期间到达的
    // 步进一律丢弃（不排队、不累加）。
    //  · seekInFlight_ = 有 seek 在途（发出即置位，PLAYBACK_RESTART 清除）
    //  · seekIssueMs_  = 发出时刻（steady clock ms，仅作不可 seek 流的放行兜底）
    std::atomic<bool> seekInFlight_{false};
    std::atomic<int64_t> seekIssueMs_{0};
    bool pausePrev_ = false;         // pause 观察器翻转检测，初始通知 false→false 不触发
    // 渲染帧率差值基准：(renderedFrames_ 增量 / Δt)，osdTextString 计算用
    // （osdTextString 在事件线程调用——fpsTimer_/fpsPrevRendered_ 仅事件线程访问）
    int64_t fpsPrevRendered_ = 0;
    QElapsedTimer fpsTimer_;
    // ── OSD 状态（mpv 内部 osd-msg1 渲染）───────────────────────────
    // 事件线程 idle 回调拉取 osdTextString 并推送（benchmark 同方案）；
    // 主线程 toggleOsd 只翻转 osdVisible_/osdLevel_（atomic）。
    std::atomic<bool> osdVisible_{false};
    std::atomic<bool> stereo3d_{false};   // 3D 激活（OSD 改走 QML 覆盖层）
    mutable std::mutex osdTextMu_;
    QString osdText_;                     // 最近一次 OSD 文本（3D 的 QML 读）
    std::atomic<bool> osdLevel_{false};       // true=level 1（显示）
    bool osdLevelApplied_ = false;            // 事件线程专用：上次应用的 level
    std::string osdLastText_;                 // 事件线程专用：上次推送的文本
    bool fullscreen_ = false;
    std::atomic<int> quality_{3};
    std::atomic<int> denoise_{-1};
    std::atomic<double> scale_{0.0};
    std::atomic<int> frucFps_{-1};      // 档位：-1 off | VFG 2/3/4 倍率 |
                                        // RIFE 40/48/60 目标帧率（RIFE 另接受
                                        // benchmark 2/3/4 倍率；数值自区分，
                                        // 正常/benchmark 共用，见 FrucBackend.h）
    std::atomic<bool> frucOrderPre_{false};  // vf 链前置插帧（仅 VFG，--fruc-order pre）
    // 插帧质量档：0 low / 1 medium / 2 high（仅 VFG 生效；默认 1 = medium，
    // 依据见 FrucBackend.h kModeDefault）。RIFE 分支不发送该参数。
    std::atomic<int> frucQuality_{1};
    // 输出帧率上限（仅 VFG）：48/60/120，默认 120（见 FrucBackend.h）。
    // 以倍率插帧后 k*src_fps 超过它 → filter 该帧直通（reason=max-fps）。
    std::atomic<int> frucMaxFps_{fruc::kMaxFpsDefault};
    std::mutex frucStatusMtx_;
    std::string frucStatus_;            // 最新 FRUC 状态行（OSD 显示；前缀随后端）
    std::string vsrStatus_;             // 最新 vsr 状态行（OSD 显示）
    std::mutex  vsrStatusMtx_;
    std::string hdrStatus_;             // HDR 上屏状态行（OSD 显示）
    std::mutex  hdrStatusMtx_;
    std::string hdrOutput_;             // 实际输出模式 HDR10/SDR（OSD"输出"行）
    std::mutex  hdrOutputMtx_;
    int loopMode_ = 0;           // 0 none, 1 loop-file, 2 loop-playlist
    PlaylistModel playlistModel_;   // 播放列表镜像（观察器 setSnapshot 增量喂入）
    QString gpuName_;

    // ── idle 重播（stop/正常播完 → 播放键从头重播）───────────────
    // 主线程状态，由事件线程的 "path" 属性观察回写：path 非空 →
    // 有活动文件；为空（停止/播完/未加载）→ fileLoaded_=false，
    // 播放按钮复位，togglePlayPause 检测后从 lastPath_ 重新加载。
    bool fileLoaded_ = false;
    // 直播/线性流标记按**路径**记（同一文件被 mpv 重新加载时不清除；
    // 换文件后 lastPath_ 变化即自然失效）。
    QString liveSourcePath_;
    bool    liveSourcePending_ = false;
    QString lastPath_;
    /// 当前蓝光 ISO（mpv 的 bluray-device 是全局选项）：bd:// 条目离开它
    /// 就打不开，所以随播放列表一起持久化，启动恢复前先设回去。
    QString blurayDevice_;
    /// 上一次 playlist 快照的条目数（判断"非空 → 空"的跃迁；启动时的
    /// 初始空快照不能当成"用户清空了列表"）。
    int playlistCount_ = 0;
    // 启动 idle：列表已排队但**没有 current 条目**（媒体未载入）。记住
    // 播放键第一次要载入的目标索引；同时供 playlist 观察器在 mpv 报
    // cur<0（无 current）时高亮"按播放将载入的那一条"。
    // 一旦真正载入播放，mpv 报 cur>=0 → 自然失效（并在消费后清零）。
    int idlePlayIndex_ = -1;

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
    int persistFrucQuality_ = 1;  // 持久化插帧质量档（initFrucQuality 用；1=medium）
    int persistFrucMaxFps_ = fruc::kMaxFpsDefault;  // 持久化输出帧率上限（initFrucMaxFps 用）
    int persistDeintMode_ = 0;    // 持久化反交错模式（initDeinterlace 用）
    std::atomic<int> deintMode_{0};   // 当前反交错模式（0/1/2；RPC 线程读）
    int persistRotMode_ = 0;          // 持久化旋转角度（0/90/270）
    std::atomic<int> rotMode_{0};    // 当前旋转角度（RPC 线程读）
    std::atomic<int> stereoLayout_{-1};  // 3D 布局（-1=非3D, 0=SBS, 1=上下；RPC 线程读）
    std::atomic<int> stereoHalf_{0};     // 3D 全/半（0=全 1=半）
    // 单眼 2D 输出（vf_eye）：0=off / 1=left / 2=right。运行期状态、不持久化。
    // 布局/全半与 3D 共用语义（J/L），但**与 stereoLayout_ 分开存**：3D 关闭时
    // stereoLayout_ 是 -1（vf_vsr 走 2D 公式），而 vf_eye 仍需要真实布局。
    std::atomic<int> eyeMode_{0};
    std::atomic<int> eyeLayout_{0};      // 0=SBS 1=上下
    std::atomic<int> eyeHalf_{0};        // 0=全 1=半
};
