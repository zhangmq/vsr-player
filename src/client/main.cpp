/// VSR Player — Qt Quick client (single wl_surface overlay UI).
/// Uses Qt's Vulkan resources (QRhi) for zero-copy CUDA-Vulkan interop.
/// QML overlays render on the same wl_surface as Vulkan video.

#include <cstdio>
#include <cstring>
#include <memory>

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

#include <vulkan/vulkan.h>

// Generated SPIR-V shaders
#include "video_vert_spv.h"
#include "video_frag_spv.h"
#include "nv12_frag_spv.h"

#include "api/Player.h"
#include "PlaylistEngine.h"
#include "QtVulkanContext.h"
#include "PlayerViewModel.h"
#include "KeyFilter.h"

namespace vsr {

// ── Helper: create a Vulkan render pass compatible with Qt's swapchain ──

static VkRenderPass makeRenderPass(VkDevice dev, QVulkanDeviceFunctions* vkdf) {
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_B8G8R8A8_UNORM;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference r{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription s{};
    s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    s.colorAttachmentCount = 1;
    s.pColorAttachments = &r;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &a;
    ci.subpassCount = 1; ci.pSubpasses = &s;

    VkRenderPass rp = VK_NULL_HANDLE;
    vkdf->vkCreateRenderPass(dev, &ci, nullptr, &rp);
    return rp;
}

}  // namespace vsr

// ── Entry point ────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    qputenv("QT_VULKAN_DEVICE_EXTENSIONS",
            "VK_KHR_external_memory;VK_KHR_external_memory_fd");
    qputenv("QSG_RHI_BACKEND", "vulkan");

    // ── Qt app ──────────────────────────────────────────────────────

    QGuiApplication app(argc, argv);
    app.setApplicationName("VSR Player");

    // ── CLI parsing ─────────────────────────────────────────────────

    bool no_hwaccel = false;
    int quality = 3;
    int scale = 0;
    int denoise = -1;
    int scanDepth = 3;
    QString file;

    QCommandLineParser parser;
    parser.setApplicationDescription("VSR Player");
    parser.addHelpOption();

    QCommandLineOption scaleOption("scale", "Super-resolution scale (off, auto, 2x, 3x, 4x)", "scale", "auto");
    parser.addOption(scaleOption);

    QCommandLineOption denoiseOption("denoise", "Force denoise quality (off, low, medium, high, ultra)", "denoise", "off");
    parser.addOption(denoiseOption);

    QCommandLineOption qualityOption("quality", "Upscale quality (low, medium, high, ultra)", "quality", "high");
    parser.addOption(qualityOption);

    QCommandLineOption depthOption("depth", "Folder scan depth", "depth", "3");
    parser.addOption(depthOption);

    QCommandLineOption noHwaccelOption("no-hwaccel", "Disable NVDEC hardware acceleration");
    parser.addOption(noHwaccelOption);

    parser.addPositionalArgument("file", "Video file or folder to play");

    parser.process(app);

    // --scale mapping
    {
        QString s = parser.value("scale").toLower();
        if (s == "off" || s == "关闭") scale = -1;
        else if (s == "auto" || s == "自动") scale = 0;
        else if (s == "2x" || s == "2") scale = 2;
        else if (s == "3x" || s == "3") scale = 3;
        else if (s == "4x" || s == "4") scale = 4;
    }

    // --denoise mapping
    {
        QString s = parser.value("denoise").toLower();
        if (s == "off" || s == "关闭") denoise = -1;
        else if (s == "low") denoise = 8;
        else if (s == "medium") denoise = 9;
        else if (s == "high") denoise = 10;
        else if (s == "ultra") denoise = 11;
    }

    // --quality mapping
    {
        QString s = parser.value("quality").toLower();
        if (s == "low") quality = 1;
        else if (s == "medium") quality = 2;
        else if (s == "high") quality = 3;
        else if (s == "ultra") quality = 4;
    }

    no_hwaccel = parser.isSet("no-hwaccel");

    {
        bool ok = false;
        scanDepth = parser.value("depth").toInt(&ok);
        if (!ok || scanDepth < 1) scanDepth = 3;
    }

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) file = args.first();

    // ── QQuickView ──────────────────────────────────────────────────

    QQuickView view;
    view.setTitle("VSR Player");
    view.setMinimumSize({800, 600});
    view.setColor(QColor(10, 10, 10));

    // ── Controller + PlaylistEngine + KeyFilter ────────────────────

    view.rootContext()->setContextProperty("window", &view);  // QQuickView* → QML's 'window'

    vsr::PlayerViewModel viewModel;
    view.rootContext()->setContextProperty("viewModel", &viewModel);

    PlaylistEngine playlist;
    view.rootContext()->setContextProperty("playlist", &playlist);

    auto* keyFilter = new vsr::KeyFilter(&viewModel, &playlist, &view);
    view.installEventFilter(keyFilter);

    // ── Scan folder / load file ─────────────────────────────────────

    if (!file.isEmpty()) {
        QFileInfo fi(file);
        if (fi.isDir()) {
            playlist.scanFolder(file, scanDepth);
        } else {
            playlist.scanFolder(file, scanDepth);
        }
    }

    // ── Player init via beforeRenderPassRecording ───────────────────

    std::unique_ptr<vsr::Player> player;
    VkRenderPass compatRp = VK_NULL_HANDLE;
    bool ready = false;
    bool initAttempted = false;
    int lastSentW = 0, lastSentH = 0;
    vsr::QtVulkanContext* qtVk = nullptr;

    QObject::connect(&view, &QQuickWindow::beforeRenderPassRecording, [&]() {
        auto* rif = view.rendererInterface();
        if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan) return;

        void* r = rif->getResource(&view, QSGRendererInterface::CommandListResource);
        if (!r) return;
        VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(r);
        if (!cb) return;

        int w = view.size().width() * view.devicePixelRatio();
        int h = view.size().height() * view.devicePixelRatio();

        // One-time init with Qt's Vulkan resources
        if (!initAttempted) {
            initAttempted = true;

            r = rif->getResource(&view, QSGRendererInterface::VulkanInstanceResource);
            if (!r) return;
            auto* vi = static_cast<QVulkanInstance*>(r);

            r = rif->getResource(&view, QSGRendererInterface::DeviceResource);
            if (!r) return;
            VkDevice dev = *static_cast<VkDevice*>(r);

            r = rif->getResource(&view, QSGRendererInterface::PhysicalDeviceResource);
            if (!r) return;

            r = rif->getResource(&view, QSGRendererInterface::CommandQueueResource);
            if (!r) return;

            auto* vkdf = vi->deviceFunctions(dev);
            if (!vkdf) return;

            compatRp = vsr::makeRenderPass(dev, vkdf);
            if (!compatRp) {
                fprintf(stderr, "Failed to create compatible render pass\n");
                return;
            }

            // Client owns IVulkanContext; core owns VulkanRenderer
            qtVk = new vsr::QtVulkanContext(&view);
            qtVk->setQueueFamily(0);
            qtVk->setRenderPass(compatRp);
            qtVk->initCommandPool();

            player = vsr::CreatePlayer();
            viewModel.setPlayer(player.get());

            // Event callback: marshal to main thread via QueuedConnection
            player->set_event_callback([&](const vsr::PlayerEvent& e) {
                QMetaObject::invokeMethod(qApp, [&, e] {
                    switch (e.type) {
                    case vsr::PlayerEvent::VIDEO_INFO:
                        viewModel.updateState(true);
                        viewModel.updateVsrActive(e.vsr_active);
                        viewModel.updateHwDecoding(e.hw_decoding);
                        player->send_command(vsr::CmdPlay{});
                        break;
                    case vsr::PlayerEvent::STATE_CHANGED:
                        viewModel.updateState(e.state == vsr::PlaybackState::PLAYING);
                        break;
                    case vsr::PlayerEvent::POSITION_CHANGED:
                        viewModel.updateTime(e.time_ms, e.duration_ms);
                        break;
                    case vsr::PlayerEvent::ERROR:
                        fprintf(stderr, "Player error: %s\n", e.error_msg.c_str());
                        break;
                    case vsr::PlayerEvent::END_OF_FILE:
                        viewModel.updateState(false);
                        {
                            QString nextFile = playlist.next();
                            if (!nextFile.isEmpty()) viewModel.loadFile(nextFile);
                        }
                        break;
                    default: break;
                    }
                }, Qt::QueuedConnection);
            });

            auto* p = player.get();
            if (p->initialize(qtVk,
                    reinterpret_cast<const uint32_t*>(video_frag_spv),
                    video_frag_spv_len,
                    reinterpret_cast<const uint32_t*>(nv12_frag_spv),
                    nv12_frag_spv_len,
                    reinterpret_cast<const uint32_t*>(video_vert_spv),
                    video_vert_spv_len,
                    quality, no_hwaccel)) {
                ready = true;

                viewModel.setScale(scale);
                viewModel.setQuality(quality);
                viewModel.setDenoiseQuality(denoise);

                QString firstFile = playlist.currentFile();
                if (firstFile.isEmpty() && !file.isEmpty()) firstFile = file;
                if (firstFile.isEmpty()) {
                    fprintf(stderr, "No video file to play\n");
                    return;
                }

                lastSentW = w; lastSentH = h;
                player->send_command(vsr::CmdResize{w, h});
                viewModel.loadFile(firstFile);
            } else {
                fprintf(stderr, "Player::initialize() failed\n");
            }
        }

        if (!ready) return;

        // Send raw resize every frame — core debounces
        {
            int w = view.size().width() * view.devicePixelRatio();
            int h = view.size().height() * view.devicePixelRatio();
            if (w != lastSentW || h != lastSentH) {
                lastSentW = w; lastSentH = h;
                player->send_command(vsr::CmdResize{w, h});
            }
        }

        auto* p = player.get();
        if (!p) return;

        // Clear background to dark color (~#0a0a0a)
        {
            r = rif->getResource(&view, QSGRendererInterface::DeviceResource);
            VkDevice dev = *static_cast<VkDevice*>(r);
            r = rif->getResource(&view, QSGRendererInterface::VulkanInstanceResource);
            auto* vkdf = static_cast<QVulkanInstance*>(r)->deviceFunctions(dev);

            VkClearAttachment ca{};
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = 0;
            ca.clearValue.color.float32[0] = 0.04f;
            ca.clearValue.color.float32[1] = 0.04f;
            ca.clearValue.color.float32[2] = 0.04f;
            ca.clearValue.color.float32[3] = 1.0f;

            VkClearRect cr{};
            cr.layerCount = 1;
            cr.rect.extent = {(uint32_t)w, (uint32_t)h};
            vkdf->vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }

        p->record_frame(cb, w, h);
        view.update();
    });

    // ── Load QML overlay ────────────────────────────────────────────

    view.setSource(QUrl::fromLocalFile(
        "/home/zmq/projects/vsr-player/src/client/ui/overlay.qml"));

    // Wire C++ KeyFilter::togglePlaylist → QML root object
    {
        QObject* rootObj = static_cast<QObject*>(view.rootObject());
        if (rootObj) {
            QObject::connect(keyFilter, &vsr::KeyFilter::togglePlaylist, [rootObj]() {
                QMetaObject::invokeMethod(rootObj, "togglePlaylist", Qt::AutoConnection);
            });
        }
    }

    view.show();

    int ret = app.exec();

    // Destroy QML objects before C++ context properties go out of scope,
    // preventing "TypeError: Cannot read property ... of null" on shutdown.
    view.setSource(QUrl());

    // Cleanup: core first, then client-owned Vulkan infrastructure
    if (player) {
        player->send_command(vsr::CmdQuit{});
        player->shutdown();       // joins worker, tears down core resources
    }
    if (qtVk) {
        qtVk->destroyCommandPool();
        delete qtVk;
    }

    return ret;
}
