/// vsr-player — Qt Quick client: mpv + VSR video playback
#include <clocale>
#include <cstdio>
#include <chrono>
#include <string>

#include "Log.h"

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickWindow>
#include <QQuickGraphicsDevice>
#include <QVulkanInstance>
#include <QSurfaceFormat>
#include <QTranslator>
#include <QLocale>
#include <QFile>
#include <QFontDatabase>
#include <QQmlContext>

#include "Options.h"
#include "VulkanContext.h"
#include "CompositePipeline.h"
#include "Video.h"
#include "MpvController.h"
#include "PlayerViewModel.h"
#include "RpcServer.h"

extern "C" {
VkResult vkDeviceWaitIdle(VkDevice);
}

// OSD 统一机制（benchmark 与正常播放共用）：viewModel 的 osdTextString()
// 基于 viewModel 状态计算完整文本（数据源 = 观察器填充的真实数据），
// mpv 事件线程 idle 回调（~100ms）拉取并推送 osd-msg1——OSD 由 mpv 在
// VO 合成阶段渲染（不经 Qt 场景图，不竞争主线程渲染循环）。
// 主线程调 mpv API 更新 osd-msg1 会与 untimed 渲染死锁（实测卡死）。

int main(int argc, char *argv[]) {
    setbuf(stdout, nullptr); setbuf(stderr, nullptr);
    MLOG_INFO("=== vsr-player ===");

    // ── CLI ──────────────────────────────────────────────────────────────
    Options opts;
    if (!Options::parse(argc, argv, &opts)) return 1;
    // benchmark 全静默（与 mpv msg-level all=no 对齐，日志不干扰测量）
    vsr_log_set_quiet(opts.benchmark);
    MLOG_INFO("video: %s (benchmark=%d)", opts.video_file.c_str(), opts.benchmark);

    qputenv("QSG_RHI_BACKEND", "vulkan");
    // basic 渲染循环：updatePaintNode/mpv work 在 GUI 线程同步执行
    //（Video.h 原始设计假设 "basic render loop only"）。threaded 模式
    // 下渲染线程 acquire/present 与 GUI 投递串行（双 vblank 30fps），
    // basic 无渲染线程——渲染由事件驱动、present 自然节流。
    qputenv("QSG_RENDER_LOOP", "basic");

    // ── vsync（运行期设置，benchmark 强制关闭）─────────────────────────
    // swapInterval(0)=off（MAILBOX 非阻塞 present），(1)=on（FIFO）。
    // 与 QSG_NO_VSYNC 环境变量等价（qsgdefaultcontext.cpp），通过 API 控制。
    QSurfaceFormat fmt;
    fmt.setSwapInterval(opts.vsync ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(fmt);
    MLOG_INFO("vsync: %s", opts.vsync ? "on" : "off");

    // ── Vulkan（shared instance/device for mpv + Qt）───────────────────
    VulkanContext vk;
    if (!vk.init()) return 1;
    MLOG_INFO("shared VkDevice ready (qfi=%u)", vk.queueFamilyIndex());

    // ── Qt ──────────────────────────────────────────────────────────────
    QGuiApplication app(argc, argv);
    setlocale(LC_NUMERIC, "C");
    QVulkanInstance qtVi; qtVi.setVkInstance(vk.instance()); qtVi.create();
    MLOG_INFO("QGuiApplication ready");

    // ── i18n：--lang 覆盖，否则跟随系统 locale（前缀回退 en）─────────
    // .qm 由 meson 生成到 VSR_TRANSLATIONS_DIR（构建目录）；未来打包
    // 时改为 <app>/translations。
    QTranslator translator;
    {
        QString lang = opts.lang.empty()
            ? QLocale::system().name()
            : QString::fromStdString(opts.lang);
        QString dir = QString::fromLatin1(VSR_TRANSLATIONS_DIR);
        if (!QFile::exists(dir))
            dir = QCoreApplication::applicationDirPath() + "/translations";
        bool ok = translator.load("vsr-player_" + lang, dir)
               || translator.load("vsr-player_" + lang.left(2), dir);
        if (ok) {
            app.installTranslator(&translator);
            MLOG_INFO("translation: vsr-player_%s (%s)", lang.toUtf8().constData(),
                 dir.toUtf8().constData());
        } else if (!opts.lang.empty()) {
            MLOG_WARN("no translation for '%s'", opts.lang.c_str());
        }
    }

    // ── VSR 参数 → vf string（viewModel 单一来源）────────────────────
    PlayerViewModel viewModel;
    viewModel.initVsr(opts.scale, opts.quality, opts.denoise);
    std::string vf = viewModel.vfOption();

    // ── mpv ─────────────────────────────────────────────────────────────
    MpvController mpv;
    int num_dev_exts = 0;
    const char *const *dev_exts = vk.deviceExtensions(&num_dev_exts);
    if (!mpv.init(vk.instance(), vk.physicalDevice(), vk.device(),
                  vk.queueFamilyIndex(), opts.benchmark, opts.hwaccel,
                  vf.c_str(), opts.passthrough, vk.features(),
                  dev_exts, num_dev_exts))
        return 1;
    MLOG_INFO("mpv init done (num_dev_exts=%d)", num_dev_exts);

    if (!opts.screenshot_dir.empty())
        mpv.setPropertyString("screenshot-directory", opts.screenshot_dir);

    qmlRegisterType<Video>("VSR", 1, 0, "Video");

    {
        // viewModel 声明于 main 作用域（见上方初始化处），析构晚于 mpv
        //（main 作用域先声明的对象后析构）；stopEvents 在块内显式停止，
        // 事件线程的 [this] lambda 捕获 viewModel 仍指向存活对象。
        viewModel.setGpuName(vk.deviceName());
        viewModel.attach(&mpv);          // 注册属性观察（须在 startEvents 前）

        QQuickView view;
        view.setVulkanInstance(&qtVi);
        view.setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
            vk.physicalDevice(), vk.device(), vk.queueFamilyIndex()));
        view.setColor("#202020");
        view.setTitle("vsr-player");
        // 默认 SizeViewToRootObject 会用 QML 根对象 implicitSize（500x500）
        // 覆盖 resize() → 窗口/RT 尺寸错误、scale=auto 决策失真、fps 虚高。
        // SizeRootObjectToView：根对象跟随窗口尺寸（Video anchors.fill）。
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1280, 720);  // 默认窗口尺寸，对齐 client-glfw
        MLOG_INFO("after resize: %dx%d", view.width(), view.height());

        view.rootContext()->setContextProperty("viewModel", &viewModel);
        view.rootContext()->setContextProperty("window", &view);
        // 图标字体：随项目分发（third_party，VSR_ICON_FONT 宏注入），
        // C++ 加载并注入 family 名。注意必须是 context property——
        // qmlcachegen AOT 编译不支持动态作用域（组件内引用父级未声明
        // 属性会解析失败 → font.family 空 → 图标乱码）。
        // 安装部署 fallback：<appdir>/../share/vsr-player/fonts/（install.sh
        // 复制字体到该位置；宏路径是开发机绝对路径，安装后不可用）。
        {
            QString fontPath = QString::fromLatin1(VSR_ICON_FONT);
            int fid = QFontDatabase::addApplicationFont(fontPath);
            if (fid < 0) {
                fontPath = QCoreApplication::applicationDirPath()
                         + "/../share/vsr-player/fonts/materialdesignicons-webfont.ttf";
                fid = QFontDatabase::addApplicationFont(fontPath);
            }
            QString iconFont = fid >= 0
                ? QFontDatabase::applicationFontFamilies(fid).value(0)
                : QString();
            view.rootContext()->setContextProperty("iconFont", iconFont);
            MLOG_INFO("icon font: %s (%s)", iconFont.toUtf8().constData(),
                 fontPath.toUtf8().constData());
        }
        view.rootContext()->setContextProperty("benchmarkMode", opts.benchmark);

        view.setSource(QUrl("qrc:/main.qml"));
        if (view.status() != QQuickView::Ready) {
            for (const auto &e : view.errors())
                MLOG_ERR("QML error: %s", e.toString().toUtf8().constData());
            return 1;
        }
        view.show();
        MLOG_INFO("view.show() done");

        auto *root = view.rootObject();
        auto *video = root ? root->findChild<Video*>() : nullptr;
        if (!video) { MLOG_ERR("Video not found in QML"); return 1; }
        video->initRenderTarget(vk.device(), vk.physicalDevice(),
                                vk.queueFamilyIndex(), vk.queue());
        MLOG_INFO("video init done");

        // ── Composite pipeline（render target 由 Video 管理）───────────
        CompositePipeline comp;
        if (!comp.create(vk.device(), video->descriptorSetLayout())) return 1;
        MLOG_INFO("composite pipeline ready");

        // ── Wire Video ────────────────────────────────────────────────
        video->setMpvController(&mpv);
        video->setFrameRenderedCallback([&viewModel]() {
            viewModel.notifyFrameRendered();
        });
        video->setCompositeObjects(comp.pipeline(), comp.pipelineLayout());
        mpv.setUpdateCallback([](void *data) {
            auto *v = (Video*)data;
            // setUpdateCallback 注册时若 context 有 pending updates 会立即
            // 同步调用回调——UI-only 模式（无 Video）下 data 为 nullptr。
            if (!v) return;
            using namespace std::chrono;
            double t = duration<double>(steady_clock::now().time_since_epoch()).count();
            v->lastCb_.store(t, std::memory_order_release);
            v->renderRequested_.store(true, std::memory_order_release);
            // 社区标准模式：invokeMethod(QueuedConnection) 跨线程驱动渲染
            //（Qt 官方元对象队列，非裸 postEvent）。mpv 核心线程 → GUI。
            QMetaObject::invokeMethod(v, "requestRender", Qt::QueuedConnection);
        }, video);
        MLOG_INFO("update_callback set");

        MLOG_INFO("Video wired");

        // ── 事件线程 ─────────────────────────────────────────────────
        // benchmark：END_FILE（EOF/error）→ 命令行 summary → 退出。
        // 非 benchmark：END_FILE 不处理——mpv 播放列表自动续播。
        mpv.startEvents([&viewModel, video, &app, &opts](mpv_event *ev) {
            // 文件加载完成 → 主线程重同步播放状态（事件线程读属性）。
            // 修复曲目切换后播放/暂停 UI 卡在错误状态（path 清空窗口）。
            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                viewModel.onFileLoadedFromEventThread();
                return;
            }
            if (ev->event_id != MPV_EVENT_END_FILE)
                return;
            auto *ef = (mpv_event_end_file *)ev->data;
            if (ef->reason != MPV_END_FILE_REASON_EOF &&
                ef->reason != MPV_END_FILE_REASON_ERROR)
                return;
            if (!opts.benchmark)
                return;
            double t = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double elapsed = t - video->benchT0_.load();
            int frames = video->benchFrames_.load();
            // 单次 fprintf 输出整个 summary——多线程 stderr 下多行
            // 分开打印会被 mpv 日志穿插破坏格式。
            char buf[256];
            int len = snprintf(buf, sizeof(buf),
                "\n=== Benchmark ===\nframes:    %d\ntime:      %.1fs\n",
                frames, elapsed);
            if (elapsed > 0 && frames > 0)
                len += snprintf(buf + len, sizeof(buf) - len,
                                "throughput: %.1f fps\n", frames / elapsed);
            fwrite(buf, 1, len, stderr);
            fflush(stderr);
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit,
                                      Qt::QueuedConnection);
        });
        MLOG_INFO("event thread started");

        // ── OSD 初始化（统一机制：viewModel osdTextString + 事件线程拉取）──
        // OSD 由 mpv 在 VO 合成阶段渲染（osd-msg1，不经 Qt 场景图——
        // QML OSD 每秒文本重绘曾与渲染循环竞争 → VO 误判 drop 每秒 +1）。
        // 样式两模式统一（benchmark 与正常播放同一套）。
        mpv.setPropertyString("osd-font-size", "14");
        mpv.setPropertyString("osd-font", "monospace");
        mpv.setPropertyString("osd-align-x", "left");
        mpv.setPropertyString("osd-align-y", "top");
        mpv.setPropertyString("osd-margin-x", "16");
        mpv.setPropertyString("osd-margin-y", "64");
        if (opts.benchmark) {
            // benchmark 无 UI：启动即显示 OSD（渲染 fps/帧数等由
            // osdTextString 提供，与正常播放同一文本源）
            viewModel.showOsd();
        } else {
            // 正常播放：初始隐藏（osd-level 0），Tab 切换显示
            mpv.setPropertyString("osd-level", "0");
        }

        // ── RPC server（JSON IPC，Unix socket）──────────────────────────
        RpcServer rpc;
        if (opts.rpc) {
            rpc.setMpv(&mpv);
            rpc.setViewModel(&viewModel);
            rpc.setQuitCallback([&app]() {
                // Cross-thread: RPC thread → Qt event loop.
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit,
                                          Qt::QueuedConnection);
            });
            rpc.start(opts.rpc_socket);
        }
        MLOG_INFO("rpc %s", opts.rpc ? "started" : "disabled");

        video->kickstart();

        // loadFile 前先告知 mpv RT 尺寸（SKIP_RENDERING pass，对齐 client-glfw）。
        // vf_vsr 的 scale=auto 依赖 VO 的 GET_DISPLAY_RES——vo=libmpv 返回的
        // 正是 RT 尺寸（窗口 framebuffer）。若无此 pass，第一帧处理时 RT 尺寸
        // 未知 → auto 退化 passthrough，VSR 不生效。
        MLOG_INFO("view size: %dx%d dpr=%.2f", view.width(), view.height(),
             view.devicePixelRatio());
        video->ensureRenderTarget((int)(view.width() * view.devicePixelRatio()),
                                  (int)(view.height() * view.devicePixelRatio()));
        mpv.skipRender(video->image(), VK_FORMAT_R8G8B8A8_UNORM,
                       video->width(), video->height());
        MLOG_INFO("skip render pass done (%dx%d)", video->width(), video->height());

        mpv.loadFile(opts.video_file.c_str());
        MLOG_INFO("loadFile, entering app.exec()");

        app.exec();

        // 摘除 mpv update 回调（必须在 QML 场景析构之前）：mpv VO 线程
        // 仍可能触发回调 → invokeMethod("requestRender") 打在正在销毁的
        // Video 对象上（use-after-free，退出崩溃根因，core 回溯实证）。
        // vo_libmpv 的 update() 在锁下检查回调非空，摘除后为安全 no-op。
        mpv.clearUpdateCallback();

        // 停止事件线程（viewModel 仍存活——事件线程以 [this] 捕获它；
        // OSD 拉取推送随之停止）
        mpv.stopEvents();
        vkDeviceWaitIdle(vk.device());
        MLOG_INFO("app.exec() returned, cleaning up");
    }

    mpv.destroy();
    viewModel.detach();
    return 0;
}
