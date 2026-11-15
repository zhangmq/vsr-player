#pragma once

#include <QCache>
#include <QImage>
#include <QObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class QTimer;

struct mpv_handle;
struct mpv_render_context;
struct mpv_event;

/// 进度条缩略图（seekbar thumbnail）：**进程内第二个 libmpv 实例 + 软件渲染**。
///
/// 为什么是这个形态（调研见 docs/seekbar-thumbnail-research.md）：
///  - mpv 没有内建的"进度条缩略图"功能，OSC 只定义了 `user-data/osc/draw-preview`
///    的交接口，画面要外部 thumbnailer 画；我们不是 OSC 界面，走不通。
///  - MPC-BE/PotPlayer 的"内置解码器实时解"在本项目里等价于再开一个 libmpv
///    实例（Haruna 同款），不需要新进程、不需要 ffmpeg、不需要 IPC/文件轮询。
///  - 取帧走 `MPV_RENDER_API_TYPE_SW`：CPU 渲染进内存（320×180 级别），
///    不碰 GPU/CUDA，不和主播放器的 VSR/VFG 争资源。
///
/// 线程模型（与 AGENTS.md 的"GUI 线程不得同步调 mpv API"一致）：
///  - GUI 线程只发**异步**命令（mpv_command_async；参数在调用内被复制）与
///    渲染上下文调用（mpv_render_context_update/render，约定只在 GUI 线程）。
///  - 属性读取与 `mpv_wait_event` 全部在 eventThread_（每 handle 只能一个线程）。
///  - 产出的 QImage 由 GUI 线程写、可被 RPC 线程读：用 imageMutex_ 保护。
///  - 帧就绪由渲染上下文的 update 回调驱动（VO 线程 → QueuedConnection 到
///    GUI 线程），不用 QTimer 轮询。
///
/// 请求语义（两级 seek，用户的悬停不能等 4K 精确 seek 的 0.3–0.9 s）：
///  1. 时间落到 `kBucketMs` 桶；先发 `absolute+keyframes`（10–20 ms 出图，
///     偏差最多一个 GOP）；同一桶已有图直接复用。
///  2. 悬停静止 `kExactDelayMs` 后对同一目标补 `absolute+exact` 精修，
///     到达后替换显示（并标记该桶已精确）。
class PreviewThumbnailer : public QObject {
    Q_OBJECT
public:
    struct Params {
        QString path;      ///< 空 = 无媒体（卸载）
        int width = 320;   ///< 目标宽（像素）；高度按画面宽高比算
        int eye = -1;      ///< -1 = 不裁；0 = 左/上眼；1 = 右/下眼
        int layout = 0;    ///< 0 = 左右（SBS）；1 = 上下（TAB）
        bool half = false; ///< true = 源里每眼已被压扁一半（裁完要还原宽高比）
        int rotate = 0;    ///< 菜单旋转角（叠加在文件元数据之上，交给 mpv）
        bool hdr = false;  ///< 内容 HDR → 插 tonemap 链
    };

    static constexpr qint64 kBucketMs = 2000;   ///< 缓存/请求的时间桶
    static constexpr int kExactDelayMs = 80;    ///< 静止多久后补精确 seek
    static constexpr int kCacheEntries = 64;

    explicit PreviewThumbnailer(QObject *parent = nullptr);
    ~PreviewThumbnailer() override;

    /// GUI 线程：更新参数（路径变化会重新 loadfile；其余参数变化只重建链）。
    void setParams(const Params &p);
    /// GUI 线程：请求某毫秒位置的缩略图（0..duration）。
    void request(qint64 ms);
    /// GUI 线程：指针离开进度条（丢弃未完成的目标；在途命令自然结束）。
    void cancel();

    /// 关掉整个功能（benchmark 模式）：不创建实例、忽略请求。
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    QImage image() const;
    QSize imageSize() const;
    quint64 serial() const { return serial_.load(); }

    // ── 统计（RPC/测试）──
    QString sourcePath() const;
    bool hasSource() const { return haveSource_.load(); }
    qint64 lastLatencyMs() const { return lastLatencyMs_.load(); }
    quint64 framesProduced() const { return frames_.load(); }
    quint64 cacheHits() const { return cacheHits_.load(); }
    QString lastError() const;

    /// 每次"当前请求被满足"（出新图，或命中同一桶的缓存图）+1。
    /// RPC/测试用它判断"这一请求已经有结果"，不依赖图像是否变化
    /// （同一时间桶重复请求时不会重复上传纹理）。
    quint64 servedEpoch() const { return servedEpoch_.load(); }

    /// RPC/测试用：等待 servedEpoch 超过 afterEpoch，并取出当前图。
    /// 在**非 GUI** 线程调用（内部只 sleep 在 QWaitCondition 上）。
    bool waitForServe(quint64 afterEpoch, int timeoutMs, QImage *out);

signals:
    void imageChanged();
    void stateChanged();

private:
    // ── 内部（GUI 线程）──
    bool ensureCreated();
    void destroyInstance();
    QString buildChain() const;
    void loadSource(const QString &path);
    void unloadSource();
    void applyChain();
    void applyRotate();
    void pump();
    void startSeek(bool exact);
    void onSeekWatchdog();
    void renderFrame();      // update 回调 → GUI 线程
    void publish(const QImage &img, qint64 bucket, bool exact, qint64 ms);
    void showCached(qint64 bucket);
    void clearCache();
    void setError(const QString &e);

    // ── 事件线程 ──
    void eventLoop();
    void onFileLoaded();
    void onVideoReconfig();
    void postToGui(std::function<void()> fn);

    static void updateCallbackTrampoline(void *data);

    // mpv 句柄/渲染上下文（生命周期由 ensureCreated/destroyInstance 管）
    mpv_handle *mpv_ = nullptr;
    mpv_render_context *rctx_ = nullptr;
    std::thread eventThread_;
    std::atomic<bool> eventsRunning_{false};

    // 参数（GUI 写、事件线程读 → 用互斥量；atoims 只给渲染路径）
    mutable QMutex paramsMutex_;
    Params params_;
    std::atomic<int> chainEye_{-1};
    std::atomic<int> chainLayout_{0};
    std::atomic<bool> chainHalf_{false};
    std::atomic<bool> chainHdr_{false};

    // 源信息（事件线程写）
    std::atomic<int> srcW_{0}, srcH_{0};
    std::atomic<int> srcRotate_{0};
    std::atomic<int> outW_{0}, outH_{0};   // video-out-params dw/dh（含裁剪/旋转后）
    std::atomic<bool> haveSource_{false};

    // 请求状态（GUI 线程）
    qint64 wantMs_ = -1;
    qint64 wantBucket_ = -1;
    qint64 lastSeekMs_ = -1;   ///< 上次实际发出的 seek 目标（同目标 seek 不出帧）
    int seekRetries_ = 0;      ///< 看门狗重试计数（防死循环）
    /// 参数代次：链/旋转/宽度变化会 +1。在途 seek 若属于旧代次，其帧
    /// （旧裁剪/旧尺寸）要丢弃，否则切换 3D 布局时会闪一帧错误构图。
    int seekGen_ = 0;
    int busyGen_ = -1;
    bool busy_ = false;
    bool busyExact_ = false;
    qint64 busyBucket_ = -1;
    bool awaitingFrame_ = false;
    qint64 requestStampMs_ = 0;
    qint64 shownBucket_ = -1;
    QCache<qint64, QImage> cache_;
    QSet<qint64> exactDone_;
    QTimer *exactTimer_ = nullptr;
    QTimer *seekWatchdog_ = nullptr;   ///< 在途 seek 超时兜底

    // 产物（GUI 写；RPC 线程可读）
    mutable QMutex imageMutex_;
    QImage image_;
    std::atomic<quint64> serial_{0};
    std::atomic<quint64> servedEpoch_{0};
    std::atomic<qint64> lastLatencyMs_{-1};
    std::atomic<quint64> frames_{0};
    std::atomic<quint64> cacheHits_{0};
    QWaitCondition frameReadyCond_;

    std::atomic<bool> enabled_{true};
    std::atomic<bool> createdFailed_{false};
    mutable QMutex errMutex_;
    QString lastError_;
};
