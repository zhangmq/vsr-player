#include "PreviewThumbnailer.h"

#include "Log.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <QDateTime>
#include <QMutexLocker>
#include <QStringList>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────
// 为什么是"进程内第二个 libmpv + 软件渲染"
//
// mpv 没有内建的进度条缩略图（0.41 上游也没有 vf=thumbnail 之类），OSC 只
// 定义了 `user-data/osc/draw-preview` 的**交接口**，画面要外部 thumbnailer
// 自己画；我们不是 OSC 界面，那条路用不上。
//
// MPC-BE/PotPlayer 那种"内置解码器实时解"在本项目里等价于再开一个 libmpv
// 实例（Haruna 同款做法）：不引入新进程、不需要 ffmpeg CLI、不需要 IPC 与
// 文件轮询，解码/旋转/色彩都由 mpv 自己处理。
//
// 取帧用 MPV_RENDER_API_TYPE_SW（video/out/libmpv_sw.c）：CPU 渲进 QImage，
// 320×180 级别单帧 ~1ms，不建第二个 GPU/CUDA 上下文，不与主播放器的
// VSR/VFG 抢显存。
//
// 渲染尺寸：SW_SIZE 决定输出大小，mpv 的 sws 负责缩放；宽高比取 VO 的
// video-out-params（dw/dh，已含裁剪与旋转），所以不做 letterbox 黑边。
// ─────────────────────────────────────────────────────────────────────

namespace {

/// 内容 HDR → SDR 的 tonemap 链（lavfi 桥）。
/// 顺序：转线性 → tonemap → 回 bt709。缩略图在 SW 渲染时统一缩到 320 级别，
/// 所以这里按全分辨率做 tonemap（单帧、每请求一次，实测成本可接受）。
constexpr const char *kTonemapChain =
    "lavfi=[zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
    "tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv]";

}  // namespace

PreviewThumbnailer::PreviewThumbnailer(QObject *parent) : QObject(parent) {
    cache_.setMaxCost(kCacheEntries);

    // 静止后才补精确 seek（用户快速扫过时不做无用的 4K 精确解码）。
    exactTimer_ = new QTimer(this);
    exactTimer_->setSingleShot(true);
    exactTimer_->setInterval(kExactDelayMs);
    connect(exactTimer_, &QTimer::timeout, this, [this] {
        if (busy_ || wantMs_ < 0)
            return;
        if (exactDone_.contains(wantBucket_))
            return;
        startSeek(true);
    });

    // 在途 seek 的看门狗：mpv 对"目标与当前位置相同"的 seek 不会产出新帧
    // （实测：链切换后对同一时间重请求 → 永远等不到 update 回调）。超时后
    // 清在途状态并重试（重试会因 lastSeekMs_ 相同而自动把目标挪开）。
    seekWatchdog_ = new QTimer(this);
    seekWatchdog_->setSingleShot(true);
    seekWatchdog_->setInterval(2000);
    connect(seekWatchdog_, &QTimer::timeout, this, &PreviewThumbnailer::onSeekWatchdog);
}

PreviewThumbnailer::~PreviewThumbnailer() {
    destroyInstance();
}

// ── 生命周期 ─────────────────────────────────────────────────────────

bool PreviewThumbnailer::ensureCreated() {
    if (mpv_)
        return true;
    if (!enabled_ || createdFailed_)
        return false;

    mpv_ = mpv_create();
    if (!mpv_) {
        setError(QStringLiteral("mpv_create failed"));
        createdFailed_ = true;
        return false;
    }

    auto opt = [this](const char *k, const char *v) {
        int e = mpv_set_option_string(mpv_, k, v);
        if (e < 0)
            MLOG_WARN("thumb: option %s=%s rejected: %s", k, v, mpv_error_string(e));
    };

    // 独立于用户的 mpv 配置：不读 mpv.conf、不加载脚本/OSC/输入绑定。
    opt("vo", "libmpv");
    opt("config", "no");
    opt("load-scripts", "no");
    opt("osc", "no");
    {
        // yt-dlp 源（YouTube 等）必须让**本实例自己**跑一次 yt-dlp：
        // stream-open-filename 在主实例里可能是 edl://（内含签名直链 + 请求头），
        // 复制过来也未必能开；由 mpv 自己的 ytdl_hook 解析最稳。
        // ⚠ `ytdl` 不是运行期选项，只能在创建前设 → 模式变化时重建实例。
        QMutexLocker l(&paramsMutex_);
        instanceYtdl_ = params_.ytdl;
        opt("ytdl", params_.ytdl ? "yes" : "no");
    }
    // 主实例解析出来的 HTTP 头（bilibili CDN 缺 Referer/UA 会 403）。
    // 这两项是 ytdl_hook 设的**文件级**选项，属性读得到值。
    {
        QMutexLocker l(&paramsMutex_);
        if (!params_.userAgent.isEmpty())
            opt("user-agent", params_.userAgent.toUtf8().constData());
        if (!params_.headers.isEmpty())
            opt("http-header-fields", params_.headers.toUtf8().constData());
    }
    // 预览只需要一帧：让 yt-dlp 选最低画质（thumbfast 同款），解析/下载都快
    opt("ytdl-format", "worst");
    opt("input-default-bindings", "no");
    opt("input-vo-keyboard", "no");
    opt("autoload-files", "no");
    opt("sub-auto", "no");
    opt("audio", "no");
    opt("sub", "no");
    opt("pause", "yes");
    opt("osd-level", "0");
    opt("osd-bar", "no");
    opt("force-window", "no");
    opt("terminal", "no");
    opt("msg-level", "all=warn");
    // 取帧只需要"跳到目标、解一帧"：读前缓存给小、跳过非参考帧。
    opt("demuxer-readahead-secs", "0");
    opt("demuxer-max-bytes", "128KiB");
    opt("vd-lavc-skiploopfilter", "all");
    opt("vd-lavc-fast", "yes");
    opt("vd-lavc-threads", "2");
    opt("hwdec", "no");
    opt("sws-scaler", "fast-bilinear");

    int e = mpv_initialize(mpv_);
    if (e < 0) {
        setError(QStringLiteral("mpv_initialize: %1").arg(mpv_error_string(e)));
        destroyInstance();
        createdFailed_ = true;
        return false;
    }

    const char *api = MPV_RENDER_API_TYPE_SW;
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(api)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    e = mpv_render_context_create(&rctx_, mpv_, rp);
    if (e < 0) {
        setError(QStringLiteral("render_context_create(sw): %1").arg(mpv_error_string(e)));
        destroyInstance();
        createdFailed_ = true;
        return false;
    }
    mpv_render_context_set_update_callback(rctx_, &PreviewThumbnailer::updateCallbackTrampoline,
                                           this);
    mpv_request_log_messages(mpv_, "warn");

    eventsRunning_.store(true);
    eventThread_ = std::thread(&PreviewThumbnailer::eventLoop, this);
    MLOG_INFO("thumb: instance ready (sw render, %dx%d target)", params_.width,
              params_.width * 9 / 16);
    return true;
}

void PreviewThumbnailer::destroyInstance() {
    if (rctx_)
        mpv_render_context_set_update_callback(rctx_, nullptr, nullptr);
    eventsRunning_.store(false);
    if (eventThread_.joinable()) {
        if (mpv_)
            mpv_wakeup(mpv_);
        eventThread_.join();
    }
    if (rctx_) {
        mpv_render_context_free(rctx_);
        rctx_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    haveSource_.store(false);
}

void PreviewThumbnailer::setEnabled(bool on) {
    if (enabled_ == on)
        return;
    enabled_ = on;
    if (!on) {
        wantMs_ = -1;
        wantBucket_ = -1;
        exactTimer_->stop();
        destroyInstance();
        clearCache();
        emit stateChanged();
    }
    MLOG_INFO("thumb: %s", on ? "enabled" : "disabled");
}

// ── 参数 ─────────────────────────────────────────────────────────────

QString PreviewThumbnailer::buildChain() const {
    // 只在事件/渲染之外使用：读原子量，避免和 GUI 线程的 Params 竞争。
    const int eye = chainEye_.load();
    const int layout = chainLayout_.load();
    const bool half = chainHalf_.load();

    QStringList parts;
    if (eye >= 0) {
        // SBS：左/右半；TAB：上/下半。half（源里每眼已被压扁一半）裁完
        // 要按 split 轴拉回 2 倍，否则缩略图是"宽扁"的。
        // half：源里每眼被压扁一半，裁完要按 split 轴拉回 2 倍。⚠ 必须
        // setsar=1：crop 会把像素宽高比改成 1:2 来保持显示比，直接 scale
        // 会让 mpv 的 dw/dh 再乘一次（实测 640x360 变 640x720）。
        if (layout == 0) {
            parts << QStringLiteral("crop=iw/2:ih:%1:0").arg(eye ? QStringLiteral("iw/2")
                                                                 : QStringLiteral("0"));
            if (half)
                parts << QStringLiteral("scale=iw*2:ih") << QStringLiteral("setsar=1");
        } else {
            parts << QStringLiteral("crop=iw:ih/2:0:%1").arg(eye ? QStringLiteral("ih/2")
                                                                 : QStringLiteral("0"));
            if (half)
                parts << QStringLiteral("scale=iw:ih*2") << QStringLiteral("setsar=1");
        }
    }
    if (chainHdr_.load())
        parts << QString::fromLatin1(kTonemapChain);
    return parts.join(QLatin1Char(','));
}

void PreviewThumbnailer::applyChain() {
    if (!mpv_)
        return;
    const QByteArray chain = buildChain().toUtf8();
    const char *argv[] = {"vf", "set", chain.constData(), nullptr};
    int e = mpv_command_async(mpv_, 0, argv);
    if (e < 0) {
        MLOG_WARN("thumb: vf set failed: %s", mpv_error_string(e));
        return;
    }
    MLOG_INFO("thumb: vf='%s'", chain.isEmpty() ? "(none)" : chain.constData());
}

void PreviewThumbnailer::applyRotate() {
    if (!mpv_)
        return;
    int rot = 0;
    {
        QMutexLocker l(&paramsMutex_);
        rot = params_.rotate;
    }
    const std::string v = std::to_string(((rot % 360) + 360) % 360);
    const char *argv[] = {"set", "video-rotate", v.c_str(), nullptr};
    int e = mpv_command_async(mpv_, 0, argv);
    if (e < 0)
        MLOG_WARN("thumb: set video-rotate failed: %s", mpv_error_string(e));
}

void PreviewThumbnailer::setParams(const Params &p) {
    Params old;
    {
        QMutexLocker l(&paramsMutex_);
        old = params_;
        params_ = p;
    }
    chainEye_.store(p.eye);
    chainLayout_.store(p.layout);
    chainHalf_.store(p.half);
    chainHdr_.store(p.hdr);

    if (p.path != old.path) {
        clearCache();
        shownBucket_ = -1;
        wantMs_ = -1;
        wantBucket_ = -1;
        lastSeekMs_ = -1;
        ++seekGen_;          // 旧文件在途 seek 的帧不能落到新文件上
        busy_ = false;
        awaitingFrame_ = false;
        seekWatchdog_->stop();
        exactTimer_->stop();
        if (p.path.isEmpty()) {
            unloadSource();
            emit stateChanged();
            return;
        }
        if (!ensureCreated())
            return;
        loadSource(p.path);
        return;
    }

    const bool chainChanged =
        p.eye != old.eye || p.layout != old.layout || p.half != old.half || p.hdr != old.hdr;
    if (p.path != old.path || chainChanged || p.rotate != old.rotate || p.width != old.width)
        MLOG_VERBOSE("thumb: params w=%d eye=%d layout=%d half=%d hdr=%d rot=%d path='%s'",
                     p.width, p.eye, p.layout, p.half ? 1 : 0, p.hdr ? 1 : 0, p.rotate,
                     p.path.toUtf8().constData());
    // HTTP 头变了（换站/换页）→ 必须重新 load（旧连接的请求头不会跟着变）
    const bool headersChanged =
        p.userAgent != old.userAgent || p.headers != old.headers;
    if (headersChanged && !p.path.isEmpty()) {
        loadSource(p.path);
        return;
    }
    const bool rotateChanged = p.rotate != old.rotate;
    const bool widthChanged = p.width != old.width;

    if (chainChanged || rotateChanged || widthChanged) {
        clearCache();
        ++seekGen_;   // 在途 seek 的帧属于旧代次 → 丢弃
    }
    if (chainChanged)
        applyChain();
    if (rotateChanged)
        applyRotate();
    if ((chainChanged || rotateChanged) && wantMs_ >= 0)
        pump();
}

void PreviewThumbnailer::loadSource(const QString &path) {
    bool wantYtdl = false;
    {
        QMutexLocker l(&paramsMutex_);
        wantYtdl = params_.ytdl;
    }
    if (mpv_ && instanceYtdl_ != wantYtdl) {
        // ytdl 只能在创建前设置：换了源类型就重建（只在不带 ytdl 的直链与
        // yt-dlp 源之间切换时发生，不常见；代价 ~30ms + 重新 loadfile）。
        MLOG_INFO("thumb: recreate instance (ytdl %d -> %d)", instanceYtdl_ ? 1 : 0,
                  wantYtdl ? 1 : 0);
        destroyInstance();
        if (!ensureCreated())
            return;
    }
    applyChain();
    applyRotate();
    haveSource_.store(false);
    outW_.store(0);
    outH_.store(0);
    const QByteArray p = path.toUtf8();
    const char *argv[] = {"loadfile", p.constData(), "replace", nullptr};
    int e = mpv_command_async(mpv_, 0, argv);
    if (e < 0) {
        setError(QStringLiteral("loadfile: %1").arg(mpv_error_string(e)));
        return;
    }
    // 网络源的 stream-open-filename 是自包含 `edl://`（bilibili/YouTube 实测
    // 几十 KB），整条打进日志既刷屏又没法读——只记前 96 字节 + 总长度。
    const QByteArray shown = p.size() > 96 ? p.left(96) + "…" : p;
    MLOG_INFO("thumb: load '%s' (%d bytes)", shown.constData(), int(p.size()));
}

void PreviewThumbnailer::unloadSource() {
    if (!mpv_)
        return;
    haveSource_.store(false);
    outW_.store(0);
    outH_.store(0);
    shownBucket_ = -1;
    const char *argv[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, argv);
}

// ── 请求 ─────────────────────────────────────────────────────────────

void PreviewThumbnailer::request(qint64 ms) {
    if (!enabled_)
        return;
    if (!ensureCreated())
        return;
    ms = std::max<qint64>(0, ms);
    MLOG_VERBOSE("thumb: request %.1fs", double(ms) / 1000.0);
    wantMs_ = ms;
    wantBucket_ = ms / kBucketMs;
    exactTimer_->stop();
    pump();
}

void PreviewThumbnailer::cancel() {
    wantMs_ = -1;
    if (exactTimer_)
        exactTimer_->stop();
}

void PreviewThumbnailer::pump() {
    if (!mpv_ || busy_ || wantMs_ < 0)
        return;
    if (!haveSource_.load())
        return;   // 等 FILE_LOADED

    const qint64 b = wantBucket_;
    MLOG_VERBOSE("thumb: pump want=%.3fs bucket=%lld cached=%d exact=%d",
                 double(wantMs_) / 1000.0, (long long)b, cache_.object(b) ? 1 : 0,
                 exactDone_.contains(b) ? 1 : 0);
    if (exactDone_.contains(b)) {
        if (cache_.object(b)) {
            cacheHits_.fetch_add(1);
            showCached(b);
        }
        wantMs_ = -1;
        return;
    }
    if (cache_.object(b)) {
        // 该桶已有"关键帧图"：先显示，静止后再补精修。
        cacheHits_.fetch_add(1);
        showCached(b);
        if (!exactTimer_->isActive())
            exactTimer_->start();
        return;
    }
    startSeek(false);
}

void PreviewThumbnailer::startSeek(bool exact) {
    if (!mpv_ || wantMs_ < 0 || busy_)
        return;
    qint64 target = exact ? wantMs_ : wantBucket_ * kBucketMs;
    if (target == lastSeekMs_) {
        // 同一个目标：mpv 不会重新出帧。
        //  - exact 与上一次 seek 目标相同 → 上一次就已经落在该时间上，
        //    这张图本身就是精确的，不必再 seek；
        //  - keyframe 撞上同一目标（链/宽度变了要重渲）→ 轻微挪开，
        //    逼 mpv 真的 seek 一次（0.25 s 对 2 s 的桶无感）。
        if (exact) {
            exactDone_.insert(wantBucket_);
            wantMs_ = -1;
            servedEpoch_.fetch_add(1);   // 上一次 seek 已落在该时间上
            frameReadyCond_.wakeAll();
            return;
        }
        target += 250;
    }
    const double sec = double(target) / 1000.0;
    char tbuf[32];
    std::snprintf(tbuf, sizeof tbuf, "%.3f", sec);
    const char *argv[] = {"seek", tbuf, exact ? "absolute+exact" : "absolute+keyframes", nullptr};
    int e = mpv_command_async(mpv_, 0, argv);
    if (e < 0) {
        MLOG_WARN("thumb: seek(%s) failed: %s", tbuf, mpv_error_string(e));
        busy_ = false;
        awaitingFrame_ = false;
        return;
    }
    lastSeekMs_ = target;
    busy_ = true;
    busyGen_ = seekGen_;
    busyExact_ = exact;
    busyBucket_ = wantBucket_;
    awaitingFrame_ = true;
    requestStampMs_ = QDateTime::currentMSecsSinceEpoch();
    exactTimer_->stop();
    seekWatchdog_->start();
    MLOG_VERBOSE("thumb: seek %.3f (%s) bucket=%lld", sec,
                 exact ? "exact" : "keyframes", (long long)busyBucket_);
}

void PreviewThumbnailer::onSeekWatchdog() {
    if (!busy_)
        return;
    MLOG_WARN("thumb: seek timeout (target=%.3fs, retry=%d)", double(lastSeekMs_) / 1000.0,
              seekRetries_ + 1);
    busy_ = false;
    awaitingFrame_ = false;
    if (++seekRetries_ >= 2) {
        seekRetries_ = 0;
        wantMs_ = -1;   // 两次都没出帧：放弃这次请求，保留上一张图
        return;
    }
    pump();
}

void PreviewThumbnailer::showCached(qint64 bucket) {
    QImage *c = cache_.object(bucket);
    if (!c)
        return;
    if (shownBucket_ == bucket) {
        // 同一桶的重复请求：图已经在屏幕上，不重传纹理，但要让
        // "这一请求已被满足"的计数前进（RPC/测试依赖它）。
        lastLatencyMs_.store(0);
        servedEpoch_.fetch_add(1);
        frameReadyCond_.wakeAll();
        return;
    }
    {
        QMutexLocker l(&imageMutex_);
        image_ = *c;
        serial_.fetch_add(1);
        lastLatencyMs_.store(0);
    }
    shownBucket_ = bucket;
    servedEpoch_.fetch_add(1);
    frameReadyCond_.wakeAll();
    emit imageChanged();
}

void PreviewThumbnailer::publish(const QImage &img, qint64 bucket, bool exact, qint64 stampMs) {
    if (bucket >= 0) {
        cache_.insert(bucket, new QImage(img), 1);
        if (exact)
            exactDone_.insert(bucket);
    }
    {
        QMutexLocker l(&imageMutex_);
        image_ = img;
        serial_.fetch_add(1);
        lastLatencyMs_.store(stampMs > 0 ? QDateTime::currentMSecsSinceEpoch() - stampMs : -1);
    }
    MLOG_VERBOSE("thumb: publish bucket=%lld exact=%d %dx%d latency=%lldms",
                 (long long)bucket, exact ? 1 : 0, img.width(), img.height(),
                 (long long)(stampMs > 0 ? QDateTime::currentMSecsSinceEpoch() - stampMs : -1));
    shownBucket_ = bucket;
    frames_.fetch_add(1);
    servedEpoch_.fetch_add(1);
    seekRetries_ = 0;
    if (seekWatchdog_)
        seekWatchdog_->stop();
    frameReadyCond_.wakeAll();
    emit imageChanged();

    if (wantMs_ < 0)
        return;
    if (wantBucket_ != bucket) {
        pump();   // 悬停已经移到别的时间：继续服务新目标
        return;
    }
    if (exact) {
        wantMs_ = -1;
        return;
    }
    if (!exactDone_.contains(bucket) && !exactTimer_->isActive())
        exactTimer_->start();
}

// ── 渲染（GUI 线程）──────────────────────────────────────────────────

void PreviewThumbnailer::updateCallbackTrampoline(void *data) {
    auto *self = static_cast<PreviewThumbnailer *>(data);
    if (!self)
        return;
    // VO 线程 → GUI 线程（渲染上下文约定只在 GUI 线程调用）。
    QMetaObject::invokeMethod(self, [self] { self->renderFrame(); }, Qt::QueuedConnection);
}

void PreviewThumbnailer::renderFrame() {
    if (!rctx_)
        return;
    const uint64_t flags = mpv_render_context_update(rctx_);
    if (!(flags & MPV_RENDER_UPDATE_FRAME))
        return;

    int w = 0, h = 0;
    {
        int dw = outW_.load(), dh = outH_.load();
        if (dw <= 0 || dh <= 0) {
            // VO 还没报出参数：用源尺寸 + 裁剪/旋转自己算（下一帧会纠正）。
            int sw = srcW_.load(), sh = srcH_.load();
            if (sw <= 0 || sh <= 0) {
                dw = 16;
                dh = 9;
            } else {
                if (chainEye_.load() >= 0 && !chainHalf_.load())
                    (chainLayout_.load() == 0 ? sw : sh) /= 2;
                int rot = 0;
                {
                    QMutexLocker l(&paramsMutex_);
                    rot = params_.rotate;
                }
                if (((rot + srcRotate_.load()) % 180) == 90)
                    std::swap(sw, sh);
                dw = sw;
                dh = sh;
            }
        }
        int targetW = 320;
        {
            QMutexLocker l(&paramsMutex_);
            targetW = std::clamp(params_.width, 64, 1024);
        }
        h = int(std::lround(double(targetW) * dh / dw));
        h = std::clamp(h, 32, 1024);
        w = targetW;
    }

    QImage img(w, h, QImage::Format_ARGB32);
    if (img.isNull())
        return;

    int sz[2] = {w, h};
    const char *fmt = "bgra";
    size_t stride = size_t(img.bytesPerLine());
    void *ptr = img.bits();
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_SW_SIZE, sz},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(fmt)},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, ptr},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    MLOG_VERBOSE("thumb: render %dx%d awaiting=%d out=%dx%d", w, h, awaitingFrame_ ? 1 : 0,
                 outW_.load(), outH_.load());
    int e = mpv_render_context_render(rctx_, rp);
    if (e < 0) {
        MLOG_WARN("thumb: render failed: %s", mpv_error_string(e));
        return;
    }

    // SW 渲染器不做旋转（见 src/mpv/video/out/vo_libmpv.c 的补丁）：帧原样
    // 缩放，这里按 mpv 报的 rotate（= 文件元数据 + 菜单角度）在 CPU 上转。
    // 320×180 级别的 QImage 旋转 <1ms。
    const int rot = ((srcRotate_.load() % 360) + 360) % 360;
    if (rot == 90 || rot == 180 || rot == 270) {
        QTransform tf;
        tf.rotate(rot);
        img = img.transformed(tf, Qt::FastTransformation);
    }

    if (!awaitingFrame_)
        return;   // 首帧/多余帧（例如暂停态的首帧）直接丢弃
    awaitingFrame_ = false;
    if (busyGen_ != seekGen_) {
        // 参数在 seek 期间变了（切换 3D 布局/旋转/宽度）：这帧是旧构图，丢掉
        MLOG_VERBOSE("thumb: drop stale frame (gen %d != %d)", busyGen_, seekGen_);
        busy_ = false;
        pump();
        return;
    }
    const bool exact = busyExact_;
    const qint64 bucket = busyBucket_;
    const qint64 stamp = requestStampMs_;
    busy_ = false;
    publish(img, bucket, exact, stamp);
}

// ── 事件线程 ─────────────────────────────────────────────────────────

void PreviewThumbnailer::postToGui(std::function<void()> fn) {
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

void PreviewThumbnailer::eventLoop() {
    while (eventsRunning_.load()) {
        mpv_event *ev = mpv_wait_event(mpv_, 0.05);
        switch (ev->event_id) {
        case MPV_EVENT_NONE:
            continue;
        case MPV_EVENT_FILE_LOADED:
            onFileLoaded();
            break;
        case MPV_EVENT_VIDEO_RECONFIG:
            onVideoReconfig();
            break;
        case MPV_EVENT_END_FILE: {
            // 失败要记下来：悬停气泡据此显示"无法获取"（用户 2026-11-15 要求
            // 失败时也给气泡；面板形态/尺寸不变）。正常 unload 不算失败。
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            const bool failed = ef && ef->reason == MPV_END_FILE_REASON_ERROR;
            haveSource_.store(false);
            postToGui([this, failed] {
                if (failed && lastError_.isEmpty())
                    lastError_ = QStringLiteral("load failed");
                emit stateChanged();
            });
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *m = static_cast<mpv_event_log_message *>(ev->data);
            if (m && m->level && (m->level[0] == 'e' || m->level[0] == 'w'))
                MLOG_WARN("thumb: %s: %s", m->prefix ? m->prefix : "mpv",
                          m->text ? m->text : "");
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            eventsRunning_.store(false);
            break;
        default:
            break;
        }
    }
}

void PreviewThumbnailer::onFileLoaded() {
    // 源尺寸（未旋转）与元数据旋转：video-params 是解码后的参数。
    int w = 0, h = 0, rot = 0;
    mpv_node node;
    if (mpv_get_property(mpv_, "video-params", MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_MAP) {
            for (int i = 0; i < node.u.list->num; i++) {
                const char *k = node.u.list->keys[i];
                mpv_node *v = &node.u.list->values[i];
                if (!k)
                    continue;
                if (!strcmp(k, "w") && v->format == MPV_FORMAT_INT64)
                    w = int(v->u.int64);
                else if (!strcmp(k, "h") && v->format == MPV_FORMAT_INT64)
                    h = int(v->u.int64);
                else if (!strcmp(k, "rotate") && v->format == MPV_FORMAT_INT64)
                    rot = int(v->u.int64);
            }
        }
        mpv_free_node_contents(&node);
    }
    srcW_.store(w);
    srcH_.store(h);
    srcRotate_.store(rot);
    haveSource_.store(true);
    MLOG_INFO("thumb: file loaded %dx%d rotate=%d", w, h, rot);
    postToGui([this] {
        applyChain();   // 链要在文件载入后设置（之前没有 vo_chain）
        pump();
        emit stateChanged();
    });
}

void PreviewThumbnailer::onVideoReconfig() {
    // 旋转（video-rotate）在运行期可变：视频链重建时重读一次源参数，否则
    // 我们的 CPU 旋转角会停在文件载入时的值（实测 set-rotate 后旋转丢失）。
    {
        int w = 0, h = 0, rot = 0;
        mpv_node vnode;
        if (mpv_get_property(mpv_, "video-params", MPV_FORMAT_NODE, &vnode) >= 0) {
            if (vnode.format == MPV_FORMAT_NODE_MAP) {
                for (int i = 0; i < vnode.u.list->num; i++) {
                    const char *k = vnode.u.list->keys[i];
                    mpv_node *v = &vnode.u.list->values[i];
                    if (!k)
                        continue;
                    if (!strcmp(k, "w") && v->format == MPV_FORMAT_INT64)
                        w = int(v->u.int64);
                    else if (!strcmp(k, "h") && v->format == MPV_FORMAT_INT64)
                        h = int(v->u.int64);
                    else if (!strcmp(k, "rotate") && v->format == MPV_FORMAT_INT64)
                        rot = int(v->u.int64);
                }
            }
            mpv_free_node_contents(&vnode);
        }
        if (w > 0 && h > 0) {
            srcW_.store(w);
            srcH_.store(h);
        }
        srcRotate_.store(rot);
    }

    int dw = 0, dh = 0;
    mpv_node node;
    if (mpv_get_property(mpv_, "video-out-params", MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_MAP) {
            for (int i = 0; i < node.u.list->num; i++) {
                const char *k = node.u.list->keys[i];
                mpv_node *v = &node.u.list->values[i];
                if (!k)
                    continue;
                if (!strcmp(k, "dw") && v->format == MPV_FORMAT_INT64)
                    dw = int(v->u.int64);
                else if (!strcmp(k, "dh") && v->format == MPV_FORMAT_INT64)
                    dh = int(v->u.int64);
            }
        }
        mpv_free_node_contents(&node);
    }
    outW_.store(dw);
    outH_.store(dh);
    MLOG_VERBOSE("thumb: out %dx%d", dw, dh);
}

// ── 查询（跨线程安全）───────────────────────────────────────────────

QImage PreviewThumbnailer::image() const {
    QMutexLocker l(&imageMutex_);
    return image_;
}

QSize PreviewThumbnailer::imageSize() const {
    QMutexLocker l(&imageMutex_);
    return image_.size();
}

QString PreviewThumbnailer::sourcePath() const {
    QMutexLocker l(&paramsMutex_);
    return params_.path;
}

QString PreviewThumbnailer::lastError() const {
    QMutexLocker l(&errMutex_);
    return lastError_;
}

bool PreviewThumbnailer::waitForServe(quint64 afterEpoch, int timeoutMs, QImage *out) {
    QMutexLocker l(&imageMutex_);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (servedEpoch_.load() <= afterEpoch) {
        const qint64 left = deadline - QDateTime::currentMSecsSinceEpoch();
        if (left <= 0)
            return false;
        frameReadyCond_.wait(&imageMutex_, ulong(std::min<qint64>(left, 100)));
    }
    if (out)
        *out = image_;
    return true;
}

// ── 杂项 ─────────────────────────────────────────────────────────────

void PreviewThumbnailer::clearCache() {
    cache_.clear();
    exactDone_.clear();
}

void PreviewThumbnailer::setError(const QString &e) {
    {
        QMutexLocker l(&errMutex_);
        lastError_ = e;
    }
    MLOG_ERR("thumb: %s", e.toUtf8().constData());
}
