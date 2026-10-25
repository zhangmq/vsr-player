/// vsr-player — Qt Quick client: mpv + VSR video playback
#include <clocale>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "Log.h"

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickWindow>
#include <QQuickGraphicsDevice>
#include <QVulkanInstance>
#include <QSurfaceFormat>
#include <QTranslator>
#include <QLocale>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QQmlContext>
#include <QQmlEngine>
#include <QProcess>
#include <QSettings>
#include <QTimer>

#include "Options.h"
#include "FrucBackend.h"
#include "VulkanContext.h"
#include "CudaProbe.h"
#include "CompositePipeline.h"
#include "Video.h"
#include "MpvController.h"
#include "PlayerViewModel.h"
#include "RpcServer.h"
#include "StereoController.h"
#include "SessionIdle.h"
#include "HdrOutput.h"

#include <QColorSpace>
#include <QSurfaceFormat>

extern "C" {
VkResult vkDeviceWaitIdle(VkDevice);
}


// OSD 统一机制（benchmark 与正常播放共用）：viewModel 的 osdTextString()
// 基于 viewModel 状态计算完整文本（数据源 = 观察器填充的真实数据），
// mpv 事件线程 idle 回调（~100ms）拉取并推送 osd-msg1——OSD 由 mpv 在
// VO 合成阶段渲染（不经 Qt 场景图，不竞争主线程渲染循环）。
// 主线程调 mpv API 更新 osd-msg1 会与 untimed 渲染死锁（实测卡死）。

// ── 命令行输入展开（mpv 语义）────────────────────────────────────────
// 位置参数与 `--playlist <file|->` 按出现顺序拼接；`--playlist` 的内容
// 追加在它出现的位置（mpv 的 `--playlist` 就是"把列表文件追加进参数列表"）。
// 播放列表文件 = m3u/纯文本：跳过空行与 `#` 注释行（mpv 的 playlist
// demuxer 同样处理），去掉行尾 CR；`-` = 从 stdin 读到 EOF。
// 返回 false = 播放列表文件打不开（对齐 mpv 的 "Error reading playlist"）。
static bool expandInputs(const Options &opts, std::vector<std::string> *out) {
    for (const auto &in : opts.inputs) {
        if (!in.is_playlist) {
            out->push_back(in.value);
            continue;
        }
        const size_t before = out->size();
        std::ifstream file;
        std::istream *is = nullptr;
        if (in.value == "-") {
            is = &std::cin;              // mpv `--playlist=-` 语义
        } else {
            file.open(in.value);
            if (!file) {
                MLOG_ERR("playlist: cannot open '%s'", in.value.c_str());
                return false;
            }
            is = &file;
        }
        std::string line;
        while (std::getline(*is, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;                // 空行 / m3u 注释与 #EXTINF
            out->push_back(line);
        }
        MLOG_INFO("playlist: %s -> %zu entries", in.value.c_str(), out->size() - before);
    }
    return true;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, nullptr); setbuf(stderr, nullptr);
    MLOG_INFO("=== vsr-player ===");

    // ── CLI ──────────────────────────────────────────────────────────────
    Options opts;
    if (!Options::parse(argc, argv, &opts)) return 1;
    if (opts.help) return 0;   // --help：用法已打印（Options.cpp）
    // benchmark 全静默（与 mpv msg-level all=no 对齐，日志不干扰测量）
    vsr_log_set_quiet(opts.benchmark);
    MLOG_INFO("video: %s (benchmark=%d)", opts.video_file.c_str(), opts.benchmark);

    // ── 输入列表展开（位置参数 + --playlist；mpv 语义）──────────────────
    // 读 stdin（`--playlist=-`）会阻塞到 EOF——mpv 同语义（wrapper 写完
    // URL 即关 stdin；交互式终端下别写 `--playlist=-`）。
    std::vector<std::string> entries;
    if (!expandInputs(opts, &entries)) return 1;
    if (!opts.inputs.empty())
        MLOG_INFO("inputs: %zu entries (%zu from the command line)", entries.size(), opts.inputs.size());
    if (opts.benchmark && entries.empty()) {
        MLOG_ERR("benchmark: empty input list");
        return 1;
    }

    // ── 冷启动预热：引擎内核缓存为空时，后台先把内核编译掉 ───────────────
    // 超分/插帧引擎的首次 Load 要现编译内核，缓存落在 $HOME/.nv/ComputeCache；
    // 全新 HOME 第一次实测 10.8~13.5s（build/scratch/resize/REPORT.md §2.1），
    // 且该缓存与尺寸/倍率**无关**——预暖一次之后换任何尺寸/倍率都只要 ~0.15s。
    // ⚠️ 通用 CUDA 计算**无效**（实测 torch 跑一次加法后超分 Load 仍 11301ms），
    //    必须真的把这两个引擎 Load 一次。
    // 做法：用**随包分发的命令行播放器** + 合成画面跑 4 帧——零素材、零引擎代码
    // 重复（走的就是同一套 user_filters / vsr_proc / vfg_proc）。
    // 预热期间**不载入媒体**：暂停态的海报帧同样会触发引擎 Load（实测 12s），
    // 所以"只推迟播放"是不够的，必须把 loadFile 一起推迟（见下方 loadFile 处）。
    bool engineCacheCold = false;
    QProcess warmupProc;
    bool warmupPending = false;
    {
        const QString home = qEnvironmentVariable("HOME");
        const QString cache = home + QStringLiteral("/.nv/ComputeCache");
        const QDir d(cache);
        engineCacheCold = !d.exists()
            || d.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
        if (engineCacheCold)
            MLOG_INFO("first run on this HOME: engine kernel cache is empty (%s) — "
                      "the first super-resolution engine load compiles kernels and "
                      "can take 10-15 s (one-time; later loads ~0.15 s)",
                      qUtf8Printable(cache));
    }

    // 预热的实际启动在 QGuiApplication 就绪之后（applicationDirPath 需要 app）。

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

    // ── AI 可用性：Vulkan 设备 × CUDA 设备必须是同一张卡 ────────────────
    // 超分/插帧/上变换的输出是 CUDA 帧，经外部显存零拷贝交给本进程的 Vulkan
    // VO 导入；两张卡时导入无效 = 黑屏（issue #25 的形态）。这里启动时判一次：
    // 找不到 UUID 相同的 CUDA 设备就整条链直通（降级），**不改用户的设置**，
    // 并在日志里说清楚原因与可选做法。
    CudaProbeResult cuda = cuda_probe_for_vulkan_uuid(vk.deviceUuid());
    const bool aiAvailable = cuda.matched;
    std::string aiReason;
    // 32 位十六进制（无分隔符）——透给 @hwup:hwup:cuda-uuid=
    std::string aiUuidHex;
    {
        static const char *kHex = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            aiUuidHex += kHex[vk.deviceUuid()[i] >> 4];
            aiUuidHex += kHex[vk.deviceUuid()[i] & 0xF];
        }
    }
    {
        char reason[512];
        if (aiAvailable) {
            snprintf(reason, sizeof(reason), "%s ↔ CUDA %s",
                     vk.deviceName(), cuda.matchedName.c_str());
        } else {
            snprintf(reason, sizeof(reason), "Vulkan device %s has no CUDA device with the same UUID (%s)",
                     vk.deviceName(), cuda.detail.c_str());
        }
        aiReason = reason;
    }

    // ── 全依赖版：捆绑 Qt 的插件/QML 路径（标准版无此目录 → 无副作用）──
    // 布局镜像发行版：<prefix>/lib/qt6/{plugins,qml}，Qt 库在 <prefix>/lib/。
    // Qt 插件/QML 模块自带相对 RUNPATH（$ORIGIN/../../../ 等），镜像布局下
    // 无需 patchelf 即可解析。必须在 QGuiApplication 构造前设置——平台插件
    // 在构造函数内加载。
    {
        QString exeDir = QCoreApplication::applicationDirPath();
        if (exeDir.isEmpty()) {
            const QString exe = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
            if (!exe.isEmpty()) exeDir = QFileInfo(exe).absolutePath();
        }
        if (!exeDir.isEmpty()) {
            const QString prefix = exeDir + QStringLiteral("/..");
            const QString qtPlugins = QDir(prefix + QStringLiteral("/lib/qt6/plugins")).absolutePath();
            const QString qtQml     = QDir(prefix + QStringLiteral("/lib/qt6/qml")).absolutePath();
            if (QDir(qtPlugins).exists()) {
                qputenv("QT_PLUGIN_PATH", qtPlugins.toLocal8Bit());
                MLOG_INFO("bundled Qt plugins: %s", qtPlugins.toUtf8().constData());
            }
            if (QDir(qtQml).exists()) {
                qputenv("QML2_IMPORT_PATH", qtQml.toLocal8Bit());
                MLOG_INFO("bundled Qt QML: %s", qtQml.toUtf8().constData());
            }
        }
    }

    // ── Qt ──────────────────────────────────────────────────────────────
    // 原生文件/文件夹对话框：XDG desktop portal（系统文件管理器）。
    // 必须在 QGuiApplication 构造前设置；无 portal 环境时回退 Qt 自绘。
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    QGuiApplication app(argc, argv);
    // 桌面集成：Wayland app_id / X11 WM_CLASS 钉成 "vsr-player"，与安装的
    // ~/.local/share/applications/vsr-player.desktop 同名（Qt 的
    // StartupWMClass 匹配规则）→ 任务栏图标/分组、启动器关联才对得上。
    // 须在建窗口前设置。默认值取可执行文件名，钉死可避免改名/软链启动时失配。
    QGuiApplication::setDesktopFileName(QStringLiteral("vsr-player"));
    setlocale(LC_NUMERIC, "C");
    QVulkanInstance qtVi; qtVi.setVkInstance(vk.instance()); qtVi.create();
    MLOG_INFO("QGuiApplication ready");

    // ── 冷启动预热：启动命令行播放器在后台把引擎内核编译掉 ───────────────
    //（检测在上方；此处才启动是因为 applicationDirPath 需要 app 就绪）
    if (!opts.benchmark && engineCacheCold) {
        const QString appDir = QCoreApplication::applicationDirPath();
        QString cli;
        const QString candidates[] = { appDir + QStringLiteral("/mpv-vsr"),                // 安装/包内布局
                                       appDir + QStringLiteral("/../../mpv/_build/mpv") }; // 开发树
        for (const QString &c : candidates)
            if (QFileInfo::exists(c)) { cli = c; break; }
        if (cli.isEmpty()) {
            MLOG_WARN("engine cache is cold but no CLI player next to %s — skipping prewarm",
                      qUtf8Printable(appDir));
        } else {
            warmupProc.setStandardOutputFile(QProcess::nullDevice());
            warmupProc.setStandardErrorFile(QProcess::nullDevice());
            warmupProc.start(cli, QStringList{
                QStringLiteral("--no-config"),
                QStringLiteral("--vo=null"), QStringLiteral("--ao=null"),
                // 代表性配置即可（缓存与尺寸/倍率无关）：720p 输入 × 超分 2 倍 + 插帧 ×2
                QStringLiteral("--vf=hwup,vsr:scale=2,vfg:fps=2:mode=medium"),
                QStringLiteral("--frames=4"),
                QStringLiteral("av://lavfi:testsrc=size=1280x720:rate=2") });
            warmupPending = warmupProc.waitForStarted(2000);
            if (warmupPending)
                MLOG_INFO("prewarm started in background (%s)", qUtf8Printable(cli));
            else
                MLOG_WARN("prewarm failed to start: %s", qUtf8Printable(warmupProc.errorString()));
        }
    }

    // ── i18n：--lang 覆盖，否则跟随系统 locale（前缀回退 en）─────────
    // .qm 由 meson 生成到 VSR_TRANSLATIONS_DIR（构建目录）；未来打包
    // 时改为 <app>/translations。
    QTranslator translator;
    {
        QString lang = opts.lang.empty()
            ? QLocale::system().name()
            : QString::fromStdString(opts.lang);
        // 判据用**具体 .qm 是否存在**，而不是"目录存在"：开发机构建目录可能
        // 被 clean 掉、或只生成了部分语言——只看目录会把空目录当成可用来源，
        // 结果静默回退英文（用户侧表现为"UI 没翻译"）。两种语言变体
        // （zh_CN / zh）任一存在即视为该目录可用。
        auto has_qm = [](const QString &d, const QString &lang) {
            return QFile::exists(d + "/vsr-player_" + lang + ".qm") ||
                   QFile::exists(d + "/vsr-player_" + lang.left(2) + ".qm");
        };
        QString dir = QString::fromLatin1(VSR_TRANSLATIONS_DIR);
        if (!has_qm(dir, lang))
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
    // 持久化 VSR 参数：CLI 未显式指定时用上次值（须在 initVsr 前——
    // 决定 vf 字符串）。benchmark 不读（测量口径：默认 auto/high/off）。
    if (!opts.benchmark)
        viewModel.loadSettings();
    viewModel.initVsr(opts.scale, opts.quality, opts.denoise);
    viewModel.initFruc(opts.fruc, opts.benchmark);
    viewModel.initFrucOrder(opts.fruc_order);   // vf 链序（仅 VFG 可切换）
    viewModel.initFrucQuality(opts.fruc_quality);  // 插帧质量档（仅 VFG 可选）
    viewModel.initFrucMaxFps(opts.fruc_max_fps);   // 输出帧率上限（仅 VFG 可选）
    viewModel.initTrueHdr();  // SDR→HDR10 上变换参数（持久化 + env 默认；须在
                              // installPolicy 前——策略的首次 reevaluate 要用它们）
    viewModel.initAiAvailability(aiAvailable, aiReason, aiUuidHex);  // 须在 vfOption() 之前

    // 多卡 + CUDA 0 号不是 Vulkan 那张卡：硬件解码没问题（mpv 按 UUID 匹配），
    // 但**软解码**时 hwup 历史上会落在 CUDA 0 号 → 跨卡 → 黑屏。现在 hwup
    // 已按 UUID 选卡（上面透下去的 cuda-uuid），这里只在配置可能踩坑时提醒。
    if (cuda.driverOk && cuda.deviceCount >= 2 && !cuda.dev0Matches) {
        MLOG_WARN("CUDA sees %d devices; device 0 is \"%s\" but the Vulkan device is \"%s\". "
                  "To keep decoding/AI on the Vulkan card, pin it with "
                  "CUDA_VISIBLE_DEVICES=GPU-<uuid> (uuid: the GPU: line above, or nvidia-smi -L)",
                  cuda.deviceCount, cuda.dev0Name.c_str(), vk.deviceName());
    }
    // 反交错：CLI `-- --deinterlace=no|auto|yes` 优先于持久化值（本会话生效，
    // 不写回设置）；mpv 侧仍是它自己的 `deinterlace` 选项，我们只负责下发。
    {
        int deintCli = -1;
        for (const auto &kv : opts.passthrough) {
            if (kv.first != "deinterlace") continue;
            int m = PlayerViewModel::parseDeintMode(kv.second);
            if (m >= 0) {
                deintCli = m;
            } else {
                MLOG_WARN("invalid --deinterlace=%s (no|yes|auto), ignored",
                          kv.second.c_str());
            }
        }
        viewModel.initDeinterlace(deintCli);
    }
    std::string vf = viewModel.vfOption();

    // ── mpv ─────────────────────────────────────────────────────────────
    // 启动策略（无参数启动 = 恢复上次列表）：
    //   有历史进度 → **idle 启动**：不载入媒体（只把列表排进 mpv），窗口
    //     秒开、零滤镜/零引擎工作；用户按播放时才载入，mpv 按 watch_later
    //     自动恢复到上次进度（与从列表正常启动完全同一条路径）。
    //     理由：mpv 的精确 seek 会把"关键帧→目标位置之间的帧"全部过一遍
    //     滤镜链再丢掉——实测 start=12.302（关键帧 6.25s）白跑 142 帧
    //     （VFG 570 个输出帧 ~1.3s + VSR 0.5s），海报晚 1.6s 出现。
    //   无历史进度 → 直接恢复列表并播放（无 seek，无浪费）。
    //   命令行给了文件 = 明确的播放意图 → 直接载入播放（不进 idle）。
    //   `-- --pause=yes` 仍是命令行优先（mpv 选项语义）：照旧载入并停在
    //     暂停态（海报可见）——这正是用户显式要求的。
    bool idleStart = false;
    if (!opts.benchmark && opts.inputs.empty()) {
        bool explicitPause = false;
        for (const auto &kv : opts.passthrough)
            if (kv.first == "pause") explicitPause = true;
        const QString target = viewModel.restoredCurrentFile();
        idleStart = !explicitPause && !target.isEmpty()
                 && PlayerViewModel::hasResumeState(target);
        if (idleStart)
            MLOG_INFO("resume state found → idle start (list restored, media load "
                      "deferred to Play): %s", target.toUtf8().constData());
    }

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
        viewModel.setGpuName(aiAvailable
                                 ? QString::fromUtf8(vk.deviceName())
                                 : QStringLiteral("%1 · AI unavailable")
                                       .arg(QString::fromUtf8(vk.deviceName())));
        viewModel.attach(&mpv);          // 注册属性观察（须在 startEvents 前）
        if (!opts.benchmark)
            viewModel.applyPlaybackSettings();   // 音量/倍速/循环（观察器回填 UI）

        // 3D OSD 原型：运行期状态（不持久化），在所选屏幕创建 3D OSD 窗口。
        // 声明在 view 之前 → 析构晚于 view，QML 绑定不会访问已销毁对象。
        StereoController stereo;

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
        // 窗口固定默认尺寸（不持久化几何——用户取消记录窗口大小，
        // 2026-08-06；benchmark 同为 1280x720 测量口径）。
        view.resize(1280, 720);
        MLOG_INFO("after resize: %dx%d", view.width(), view.height());

        view.rootContext()->setContextProperty("viewModel", &viewModel);
        view.rootContext()->setContextProperty("window", &view);
        view.rootContext()->setContextProperty("stereo", &stereo);
        stereo.setPlayerWindow(&view);
        // 图标字体：随项目分发（third_party，VSR_ICON_FONT 宏注入），
        // C++ 加载并注入 family 名。注意必须是 context property——
        // qmlcachegen AOT 编译不支持动态作用域（组件内引用父级未声明
        // 属性会解析失败 → font.family 空 → 图标乱码）。
        // 安装部署 fallback：<appdir>/../share/vsr-player/fonts/（install.sh
        // 复制字体到该位置；宏路径是开发机绝对路径，安装后不可用）。
        QString iconFont;
        {
            QString fontPath = QString::fromLatin1(VSR_ICON_FONT);
            int fid = QFontDatabase::addApplicationFont(fontPath);
            if (fid < 0) {
                fontPath = QCoreApplication::applicationDirPath()
                         + "/../share/vsr-player/fonts/materialdesignicons-webfont.ttf";
                fid = QFontDatabase::addApplicationFont(fontPath);
            }
            iconFont = fid >= 0
                ? QFontDatabase::applicationFontFamilies(fid).value(0)
                : QString();
            view.rootContext()->setContextProperty("iconFont", iconFont);
            MLOG_INFO("icon font: %s (%s)", iconFont.toUtf8().constData(),
                 fontPath.toUtf8().constData());
        }
        view.rootContext()->setContextProperty("benchmarkMode", opts.benchmark);
        view.rootContext()->setContextProperty("controlMode", false);

        // 桌面 control 窗口：同一份 main.qml，但 controlMode=true → 不创建
        // Video，只保留 UI。3D 时主窗口（Video/VO）去眼镜屏，control 窗口
        // 在主 UI 原来的屏幕/几何上显示，主 UI 不会消失。
        QQuickView controlView;
        controlView.setColor("#202020");
        controlView.setTitle("vsr-player");
        controlView.setResizeMode(QQuickView::SizeRootObjectToView);
        controlView.resize(1280, 720);
        controlView.rootContext()->setContextProperty("viewModel", &viewModel);
        controlView.rootContext()->setContextProperty("window", &controlView);
        controlView.rootContext()->setContextProperty("stereo", &stereo);
        controlView.rootContext()->setContextProperty("iconFont", iconFont);
        controlView.rootContext()->setContextProperty("benchmarkMode", false);
        controlView.rootContext()->setContextProperty("controlMode", true);
        controlView.setSource(QUrl("qrc:/main.qml"));
        if (controlView.status() != QQuickView::Ready) {
            for (const auto &e : controlView.errors())
                MLOG_ERR("control QML error: %s", e.toString().toUtf8().constData());
            return 1;
        }
        stereo.setControlWindow(&controlView);
        MLOG_INFO("control window ready");

        view.setSource(QUrl("qrc:/main.qml"));
        if (view.status() != QQuickView::Ready) {
            for (const auto &e : view.errors())
                MLOG_ERR("QML error: %s", e.toString().toUtf8().constData());
            return 1;
        }
        // ── HDR 上屏策略（env VSR_HDR=off|auto|hdr10；默认 auto）────────
        //   off  : 永远 SDR（= 改动前行为，且不碰色彩空间声明）
        //   auto : 内容 HDR（video-params/gamma=pq|hlg）× 显示能力 HDR
        //          （合成器 surface preferred 宽色域）才切 HDR10
        //   hdr10: 强制 HDR10（验证/用户覆盖）
        // 初始一律 SDR 起（auto/off）：只有强制模式需要 **show 之前** 设属性
        // ——Qt 只在 swapchain 创建时读一次 `_qt_sg_hdr_format`
        //（QSGRhiSupport::applySwapChainFormat，且只在 Vulkan RHI 后端生效）；
        // auto 的切换走运行期路径（hdr::applyMode：setFormat +
        // newCompatibleRenderPassDescriptor + createOrResize）。
        const hdr::Policy hdrPolicy = hdr::policyFromEnv();
        const hdr::Mode initialMode = hdrPolicy == hdr::Policy::ForceHdr10
                                    ? hdr::Mode::Hdr10 : hdr::Mode::Sdr;
        const VkFormat swapchainFmt = hdr::vkFormatForMode(initialMode);
        const VkFormat renderFmt    = hdr::renderTargetFormatForMode(initialMode);
        if (initialMode != hdr::Mode::Sdr) {
            const char *prop = hdr::qtHdrFormatProperty(initialMode);
            view.setProperty("_qt_sg_hdr_format", QString::fromLatin1(prop));
            // ⚠ **不要**在 show 之前设 QSurfaceFormat::colorSpace：Qt 会在
            // platform window 创建时据此自己建一个 `wp_color_management_surface_v1`；
            // 协议规定**一个 wl_surface 只能有一个** color management surface
            // （`wp_color_manager_v1` error surface_exists）→ 我们的模块
            // （WaylandColorFeedback，负责 luminances + 内容静态元数据）再
            // `get_surface()` 就是协议错误，**整条连接被断开、app 直接退出**（实测
            // 2026-10-17：`VSR_HDR=hdr10 input/goose_480p.webm` exit=255）。
            // 因此这里只设 `_qt_sg_hdr_format`（swapchain 格式需要它），
            // 色彩空间与描述交给 HdrOutput：首帧后 `syncTargets()` 设
            // colorSpace（此时 platform window 已存在，Qt 不会再建 surface——
            // 自动切换路径一直如此）+ 我们下发带 luminances/元数据的描述。
            MLOG_INFO("hdr: forcing HDR10 (VSR_HDR=hdr10) -- _qt_sg_hdr_format=%s, "
                      "rtImage fmt=%d, composite attachment fmt=%d "
                      "(colorspace/description are sent by the hdr policy after the first frame)",
                      prop, int(renderFmt), int(swapchainFmt));
        } else {
            MLOG_INFO("hdr: starting in SDR (policy %s) %s",
                      hdrPolicy == hdr::Policy::Auto ? "auto" : "off",
                      hdrPolicy == hdr::Policy::Auto
                          ? "-- switches to HDR10 at runtime when content is HDR and the display supports it"
                          : "-- off: never switches");
        }

        // 能力探测 + 日志（零行为变更）。必须在首帧之前接线
        // ——QRhiSwapChain 只在 afterRendering 里才绑定到窗口，且换屏要重探。
        hdr::installCapabilityProbe(&view);
        hdr::installSwapchainVerification(&view, initialMode);
        view.show();
        // 主 UI 默认放到 primary 屏：KWin 会把新窗口放到当前活动输出，
        // 用户鼠标在眼镜屏时主 UI/右键菜单会开到眼镜上。3D presentation
        // 才是唯一应该去眼镜屏的窗口。
        if (QScreen *primary = QGuiApplication::primaryScreen()) {
            if (view.screen() != primary) {
                MLOG_INFO("main window: move %s -> primary %s",
                          view.screen() ? view.screen()->name().toUtf8().constData() : "?",
                          primary->name().toUtf8().constData());
                view.setScreen(primary);
            }
        }
        // QML 的 Qt.quit() → QQmlEngine::quit() signal——文档不保证默认
        // 连接，QQuickView 场景实测不退出（"退出无效"根因，2026-08-06）。
        // 显式连接：菜单"退出"/Ctrl+Q 走标准退出路径（exec 返回 → 清理）。
        QObject::connect(view.engine(), &QQmlEngine::quit, &app,
                         &QCoreApplication::quit);
        MLOG_INFO("view.show() done");

        auto *root = view.rootObject();
        auto *video = root ? root->findChild<Video*>() : nullptr;
        if (!video) { MLOG_ERR("Video not found in QML"); return 1; }
        video->setRenderFormat(renderFmt);   // 必须在首帧（首次 ensureRenderTarget）之前
        video->initRenderTarget(vk.device(), vk.physicalDevice(),
                                vk.queueFamilyIndex(), vk.queue());
        MLOG_INFO("video init done");

        // ── Composite pipeline（render target 由 Video 管理）───────────
        CompositePipeline comp;
        if (!comp.create(vk.device(), video->descriptorSetLayout(), swapchainFmt)) return 1;
        MLOG_INFO("composite pipeline ready");

        // ── HDR 策略：内容 × 显示能力 → 运行期切换 ─────────────────────
        // 显示能力是异步到达的（窗口映射后才有 surface preferred 描述），
        // 内容 HDR 由 PlayerViewModel 观察 video-params/gamma 给出 →
        // 两者任一变化都会触发重新决策（hdr::installPolicy 内部）。
        {
            CompositePipeline *compPtr = &comp;
            Video *videoPtr = video;
            MpvController *mpvPtr = &mpv;
            hdr::ApplyHooks hooks;
            hooks.rebuildRenderPath = [compPtr, videoPtr, &vk](hdr::Mode m) {
                // composite render pass 的 attachment 必须匹配新的 swapchain 格式；
                // rtImage/mpv FBO 格式随 Video（下次 updatePaintNode 立即重建）。
                if (!compPtr->create(vk.device(), videoPtr->descriptorSetLayout(),
                                     hdr::vkFormatForMode(m))) {
                    MLOG_ERR("hdr: composite pipeline rebuild failed (%s) -- picture may be incompatible",
                             hdr::modeName(m));
                    return;
                }
                videoPtr->setRenderFormat(hdr::renderTargetFormatForMode(m));
                videoPtr->setCompositeObjects(compPtr->pipeline(), compPtr->pipelineLayout());
                MLOG_INFO("hdr: render path -> %s (composite attachment=%d rtImage=%d)",
                          hdr::modeName(m), int(hdr::vkFormatForMode(m)),
                          int(hdr::renderTargetFormatForMode(m)));
            };
            hooks.updateMpvTarget = [mpvPtr](hdr::Mode m, const hdr::DisplayInfo &di) {
                if (m != hdr::Mode::Hdr10) {
                    mpvPtr->setHdrTarget(false, 0.0f, 0.0f);
                    return;
                }
                if (di.haveLuminances && di.peakNits > 0) {
                    mpvPtr->setHdrTarget(true, di.minNits, di.peakNits);
                    MLOG_INFO("hdr: mpv target <- compositor screen=%s peak=%.1f nit min=%.2f nit "
                              "gamut_area=%.4f (%s)", di.screen.toUtf8().constData(),
                              di.peakNits, di.minNits, di.gamutArea,
                              di.reason.toUtf8().constData());
                } else {
                    MLOG_ERR("hdr: display has no luminance info (valid=%d icc_only=%d) -- mpv target "
                             "uses conservative default max=1000 nit/min=0", int(di.valid), int(di.iccOnly));
                    mpvPtr->setHdrTarget(true, 0.0f, 1000.0f);
                }
            };
            // SDR→HDR10 上变换（VFX TrueHDR，链上位于 vsr 之后）：
            // 判定在 HdrOutput 内（内容 SDR ∧ `VSR_UPHDR` opt-in ∧ 策略非 off ∧
            // 链上有滤镜；**与显示能力无关**——面板非 HDR 时输出仍是 SDR，帧由
            // mpv tone map 回 SDR），这里只转发到 `vf-command truehdr
            // enable|luminance`；被调的
            // setUpconvert 同时记住状态供 FILE_LOADED 重放（链重建后 vf-command
            // 状态会回到链串默认 enable=0）。
            hooks.updateUpconvert = [&viewModel](bool on, float autoNit, float effectiveNit) {
                viewModel.setUpconvert(on, autoNit, effectiveNit);
            };
            // OSD 的 HDR 状态行（SDR / HDR10 + 显示峰值 + 内容峰值/未上屏原因）：
            // 模式、显示能力、内容元数据任一变化都会回调 → 刷新到 OSD 文本源。
            hooks.statusChanged = [&viewModel] {
                const QString s = hdr::statusText();
                viewModel.setHdrStatus(s.toStdString());
                // OSD 的"输出"行：**实际**输出模式（真实 swapchain 格式，不是策略意图）
                viewModel.setHdrOutput(hdr::outputText().toStdString());
                // 状态变更才打日志（INFO = 状态变更；便于无 UI 时核对 OSD 来源）
                static QString last;
                if (last != s) {
                    last = s;
                    MLOG_INFO("hdr status: %s", s.toUtf8().constData());
                }
            };
            hdr::installPolicy(&view, hdrPolicy, initialMode, hooks);
            QObject::connect(&viewModel, &PlayerViewModel::videoHdrChanged, &view, [&viewModel] {
                hdr::setContentHdr(viewModel.videoHdr());
            });
            // 内容静态元数据（mastering min/max + MaxCLL/MaxFALL）→ 合成器描述的
            // "内容峰值"依据；缺它合成器只能按主色体积上限（PQ 名义 10000nit）压，
            // 真实片源（普遍 mastering≈1000nit）会被压得过狠、对比度变差。
            QObject::connect(&viewModel, &PlayerViewModel::videoHdrMetaChanged, &view, [&viewModel] {
                const PlayerViewModel::HdrMeta m = viewModel.hdrMeta();
                hdr::ContentMeta cm;
                cm.valid = m.valid;
                cm.minMastering = m.minMastering;
                cm.maxMastering = m.maxMastering;
                cm.maxCll = m.maxCll;
                cm.maxFall = m.maxFall;
                hdr::setContentMeta(cm);
            });
        }

        // ── Wire Video ────────────────────────────────────────────────
        video->setMpvController(&mpv);
        video->setStereoController(&stereo);
        video->setSourceAspectProvider([&viewModel]() {
            const int w = viewModel.videoWidth(), h = viewModel.videoHeight();
            return (w > 0 && h > 0) ? double(w) / double(h) : 0.0;
        });
        video->setFrameRenderedCallback([&viewModel]() {
            viewModel.notifyFrameRendered();
        });
        video->setCompositeObjects(comp.pipeline(), comp.pipelineLayout());
        mpv.setUpdateCallback([](void *data) {
            auto *v = (Video*)data;
            // setUpdateCallback 注册时若 context 有 pending updates 会立即
            // 同步调用回调——UI-only 模式（无 Video）下 data 为 nullptr。
            if (!v) return;
            v->renderRequested_.store(true, std::memory_order_release);
            // 社区标准模式：invokeMethod(QueuedConnection) 跨线程驱动渲染
            //（Qt 官方元对象队列，非裸 postEvent）。mpv 核心线程 → GUI。
            QMetaObject::invokeMethod(v, "requestRender", Qt::QueuedConnection);
        }, video);
        MLOG_INFO("update_callback set");

        MLOG_INFO("Video wired");

        // ── 播放期会话行为：防息屏 + 指针自动隐藏 ────────────────────
        // 两者都只在"确实在播放"时生效（playing && hasFile——mpv 的 pause
        // 属性初值会让 playing 在装载文件前短暂为 true）；UI 覆盖层可见
        //（底部/顶部热区、弹窗——QML 已写回 viewModel.overlaysVisible）期间
        // 不隐藏指针，与 mpv --cursor-autohide 的 OSC 语义一致。
        // benchmark 不参与（测量口径）。
        IdleInhibitor idleInhibitor(&view);
        CursorAutoHide cursorAutoHide(&view);
        if (!opts.benchmark) {
            auto syncSessionIdle = [&]() {
                const bool playing = viewModel.playing() && viewModel.hasFile();
                idleInhibitor.setActive(playing);
                cursorAutoHide.setEnabled(playing);
                cursorAutoHide.setSuspended(viewModel.overlaysVisible());
            };
            QObject::connect(&viewModel, &PlayerViewModel::playingChanged,
                             &app, syncSessionIdle);
            QObject::connect(&viewModel, &PlayerViewModel::overlaysVisibleChanged,
                             &app, syncSessionIdle);
            // 直播/线性流：resume 不适用 → 撤销暂停，直接跟最新
            QObject::connect(&viewModel, &PlayerViewModel::liveSourceDetected,
                             &app, [&viewModel]() {
                if (!viewModel.playing()) {
                    MLOG_INFO("live source: cancelling pause, following live edge");
                    viewModel.setPaused(false);
                }
            });
            syncSessionIdle();   // 初始状态（恢复播放时 mpv 可能已 pause）
        }

        // ── FRUC（插帧）状态行 → OSD ─────────────────────────────────
        // 滤镜每 ~0.5s 输出 MSGL_STATUS 状态行，前缀随后端（FrucBackend.h
        // 单一事实源）：VFG "vfg-status:" / RIFE "fruc-status:"；事件线程
        // 提取后存入 viewModel，osdTextString 拼入 OSD。
        mpv.setLogMessageCallback([&viewModel](const char *text) {
            const std::size_t frucPrefixLen = strlen(fruc::kStatusPrefix);
            if (strncmp(text, fruc::kStatusPrefix, frucPrefixLen) == 0) {
                viewModel.setFrucStatus(text + frucPrefixLen);
                // 调试可见性：插帧状态行同步转发 stderr（其余 status 级
                // 消息（mpv 进度行）默认过滤）。
                fprintf(stderr, "[mpv status] %s", text);
            } else if (strncmp(text, "vsr-status:", 11) == 0) {
                viewModel.setVsrStatus(text + 11);
                fprintf(stderr, "[mpv status] %s", text);
            } else if (strncmp(text, "truehdr-status:", 15) == 0) {
                // SDR→HDR10 上变换状态行（诊断/验尸用：mode + cost/fps + 输出
                // PQ 采样 nit）。OSD 的 HDR 行已带 upconvert 标记，不另设状态段。
                fprintf(stderr, "[mpv status] %s", text);
            } else if (strstr(text, "Cannot seek")) {
                // 直播/线性流：mpv 拿上次保存的位置去 seek 广播流的滑动窗口
                //（stream.c "Cannot seek backward in linear streams!" /
                // demux.c "Cannot seek in this file."）→ 按直播处理。
                viewModel.noteLiveSeekFailure();
            }
        });

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
            // seek 完成（目标帧已显示）→ 连续快进/快退门控放行下一步。
            // 必须走事件而非 `seeking` 属性观察：libmpv 对观察属性做值去重，
            // 连续 seek 时完成通知可能被吞（拖进度条后方向键卡死的根因）。
            if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                viewModel.notePlaybackRestart();
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
            rpc.setWindow(&view);   // resize 命令（逐级 resize 压测）
            rpc.setStereo(&stereo); // 3D OSD 原型（测试用 RPC）
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
        mpv.skipRender(video->image(), video->renderFormat(),
                       video->width(), video->height());
        MLOG_INFO("skip render pass done (%dx%d)", video->width(), video->height());

        // 媒体载入（冷启动预热期间推迟到预热完成；见文件上方 prewarm 注释；
        // idle 启动不载入媒体 → 不等预热，立刻恢复列表）
        bool mediaLoaded = false;
        auto loadMedia = [&]() {
            if (mediaLoaded) return;
            mediaLoaded = true;
            if (!entries.empty()) {
                // CLI/播放列表（mpv 语义）：首条 replace 载入并播放，其余
                // append（不打断首条；列表整体落进 mpv 的 playlist 属性，
                // 后续由 UI/RPC 正常管理并照常持久化）
                mpv.loadFile(entries[0].c_str());
                for (size_t i = 1; i < entries.size(); i++)
                    mpv.appendFile(entries[i].c_str());
                MLOG_INFO("%zu inputs (first: %s), entering app.exec()",
                          entries.size(), entries[0].c_str());
            } else if (idleStart) {
                viewModel.restorePlaylistIdle();         // 只填列表，不载入媒体
                MLOG_INFO("playlist restored (idle start, no media load), "
                          "entering app.exec()");
            } else if (!opts.inputs.empty()) {
                // 显式给了输入但列表为空（空播放列表文件/空 stdin）：
                // 不载入媒体、也不恢复上次列表（对齐 mpv 空列表 → idle）
                MLOG_WARN("empty input list -- no media loaded");
                MLOG_INFO("empty input list, entering app.exec()");
            } else {
                viewModel.restorePlaylist();             // 无参数 → 恢复上次列表
                MLOG_INFO("playlist restored, entering app.exec()");
            }
        };

        if (idleStart) {
            // idle 启动：只排队列表，不触发任何引擎/解码工作 → 不必等预热
            MLOG_INFO("idle start → restoring the playlist only (no media, no seek)");
            loadMedia();
        } else if (warmupPending && warmupProc.state() != QProcess::NotRunning) {
            MLOG_INFO("prewarm in flight → media load deferred until the engines are warm");
            QObject::connect(&warmupProc, &QProcess::finished, &app,
                             [&loadMedia](int code, QProcess::ExitStatus st) {
                MLOG_INFO("prewarm finished (exit=%d status=%d)", code, (int)st);
                loadMedia();
            });
            // 兜底：预热失败/卡死也不能永远不播（正常 ~13s 完成）
            QTimer::singleShot(90000, &app, [&]() {
                if (!mediaLoaded) {
                    MLOG_WARN("prewarm timeout → loading media anyway");
                    warmupProc.kill();
                    loadMedia();
                }
            });
        } else {
            if (warmupPending)
                MLOG_INFO("prewarm already finished → loading media");
            loadMedia();
        }

        app.exec();

        // 保存最后文件进度：mpv_terminate_destroy 不写 watch-later，
        // 须显式 quit-watch-later（对照 command.c cmd_quit_watch_later）。
        // app.exec 返回后 mpv 事件线程仍运行（stopEvents 在下方）——
        // 同步命令与事件线程的 mpv API 调用由 core lock 串行化，安全；
        // 防挂死兜底 = vo_libmpv flip_page 的 200ms 超时（等待 render/
        // report_swap 有界，core 不会无限持锁）。
        if (viewModel.isLiveSource()) {
            // 直播/线性流不写位置——否则下次启动会再拿陈旧 start 去 seek
            //（每次启动重演一轮 "Cannot seek backward in linear streams!"）
            mpv.commandStr("delete-watch-later-config");
            mpv.commandStr("quit");
        } else {
            mpv.commandStr("quit-watch-later");
        }
        // 按文件轨道记忆不参与退出落盘：即时落盘已随每个轨道设置动作
        // 写盘（2026-08-06：关闭落盘/启动加载生命周期排除基于文件的设置）

        // 窗口几何不再持久化（2026-08-06，用户取消记录窗口大小）

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
