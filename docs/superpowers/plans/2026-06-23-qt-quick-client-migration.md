# Qt Quick Client Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Qt Widgets client with Qt Quick overlay UI, single `vsr-player` build target, merging `src/client_quick/` into `src/client/`.

**Architecture:** `QQuickView` single `wl_surface` — Vulkan video injected via `beforeRenderPassRecording` + `record_frame()`, QML overlays (TopBar, ControlBar, CenterPlayButton, SettingsMenu, PlaylistPanel) rendered in same render pass. PlayerCore uses `initialize_external()` with Qt's VkDevice — zero core changes.

**Tech Stack:** Qt 6.7+ Quick, Vulkan (via QRhi), QVulkanInstance, C++20, Shape+PathSvg icons, Behavior/Transition animations

---

## File Change Matrix

| File | Action |
|------|--------|
| `src/client/main.cpp` | **Rewrite** — QGuiApplication + QQuickView + PlayerCore external mode |
| `src/client/overlay.qml` | **New** — Root QML scene (all overlay components) |
| `src/client/IconButton.qml` | **New** — Reusable Shape+PathSvg button |
| `src/client/IconPaths.qml` | **New** — Singleton SVG path strings (Feather Icons MIT) |
| `src/client/PlaylistEngine.h` | **New** — Folder traversal + playlist navigation |
| `src/client/PlaylistEngine.cpp` | **New** — Implementation |
| `Makefile` | **Modify** — `Qt6Widgets`→`Qt6Quick`, remove Widgets MOC/objs, remove `client_quick` targets, single `vsr-player` target |
| `src/client/MainWindow.h` | **Delete** |
| `src/client/MainWindow.cpp` | **Delete** |
| `src/client/VulkanWidget.h` | **Delete** |
| `src/client/VulkanWidget.cpp` | **Delete** |
| `src/client/PlayPauseButton.h` | **Delete** |
| `src/client/PlayPauseButton.cpp` | **Delete** |
| `src/client_quick/main.cpp` | **Delete** |
| `src/client_quick/overlay.qml` | **Delete** |

---

### Task 1: Delete Widgets files + client_quick

**Files:**
- Delete: `src/client/MainWindow.h`, `src/client/MainWindow.cpp`, `src/client/VulkanWidget.h`, `src/client/VulkanWidget.cpp`, `src/client/PlayPauseButton.h`, `src/client/PlayPauseButton.cpp`
- Delete: `src/client_quick/main.cpp`, `src/client_quick/overlay.qml`

- [ ] **Step 1: Delete Widgets client files**

```bash
rm src/client/MainWindow.h src/client/MainWindow.cpp \
   src/client/VulkanWidget.h src/client/VulkanWidget.cpp \
   src/client/PlayPauseButton.h src/client/PlayPauseButton.cpp
```

- [ ] **Step 2: Delete client_quick files**

```bash
rm src/client_quick/main.cpp src/client_quick/overlay.qml
rmdir src/client_quick  # remove empty dir
```

- [ ] **Step 3: Commit**

```bash
git add -A src/client/ src/client_quick/
git commit -m "feat: delete Qt Widgets client + client_quick — prepare for Qt Quick merge"
```

---

### Task 2: Create IconPaths.qml singleton

**Files:**
- Create: `src/client/IconPaths.qml`

- [ ] **Step 1: Write IconPaths.qml**

```qml
// IconPaths.qml — SVG path source of truth (Feather Icons, MIT)
// All icons: solid fill, 24×24 viewBox (paths scaled to 24×24).
pragma Singleton
import QtQuick

QtObject {
    readonly property string play: "M8 5v14l11-7z"
    readonly property string pause: "M6 19h4V5H6v14zm8-14v14h4V5h-4z"
    readonly property string skipBack: "M18 6L10 12l8 6V6zM6 6v12h2V6H6z"
    readonly property string skipForward: "M6 18l8-6-8-6v12zM18 6v12h2V6h-2z"
    readonly property string stop: "M6 6h12v12H6z"
    readonly property string volumeHigh: "M11 5L6 9H2v6h4l5 4V5z"
    readonly property string volumeMuted: "M11 5L6 9H2v6h4l5 4V5zM23 9l-6 6m0-6l6 6"
    readonly property string settings: "M12 15a3 3 0 100-6 3 3 0 000 6z"
    readonly property string playlist: "M4 6h16M4 12h16M4 18h16"
    readonly property string close: "M18 6L6 18M6 6l12 12"
    readonly property string fullscreen: "M8 3H5a2 2 0 00-2 2v3m14-2h3a2 2 0 012 2v3m0 2v3a2 2 0 01-2 2h-3m-6 0H5a2 2 0 01-2-2v-3"
    readonly property string screenshot: "M23 19a2 2 0 01-2 2H3a2 2 0 01-2-2V8a2 2 0 012-2h4l2-3h6l2 3h4a2 2 0 012 2z"
}
```

- [ ] **Step 2: Register singleton in qmldir**

No `qmldir` needed — register via `qmlRegisterSingletonType` in main.cpp at load time. (Handled in Task 3 when we create main.cpp.)

- [ ] **Step 3: Commit**

```bash
git add src/client/IconPaths.qml
git commit -m "feat: add IconPaths.qml — Feather Icons SVG path singleton"
```

---

### Task 3: Create IconButton.qml

**Files:**
- Create: `src/client/IconButton.qml`

- [ ] **Step 1: Write IconButton.qml**

```qml
// IconButton.qml — reusable declarative button (Shape + PathSvg).
// GPU-accelerated, no Canvas, no imperative JS.

import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property string iconPath
    property real iconSize: 22
    property color iconColor: "#c8c8c8"
    property color hoverColor: "#ffffff"
    property color pressedColor: "#aaaaaa"
    property string tooltipText: ""
    signal clicked()

    implicitWidth: iconSize + 12
    implicitHeight: iconSize + 12

    property bool _hovered: false
    property bool _pressed: false

    Shape {
        id: shape
        anchors.centerIn: parent
        width: iconSize; height: iconSize
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            id: shapePath
            fillColor: root._pressed ? root.pressedColor
                     : root._hovered ? root.hoverColor
                     : root.iconColor
            strokeColor: "transparent"
            PathSvg { path: root.iconPath }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        hoverEnabled: true
        onEntered: root._hovered = true
        onExited: { root._hovered = false; root._pressed = false }
        onPressed: root._pressed = true
        onReleased: { if (root._pressed) root.clicked(); root._pressed = false }
    }

    // Hover color transition
    Behavior on iconColor { ColorAnimation { duration: 100 } }

    // Tooltip (shown on hover)
    Text {
        visible: root._hovered && root.tooltipText !== ""
        text: root.tooltipText
        color: "#e0e0e0"
        font.pixelSize: 11
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: 4
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add src/client/IconButton.qml
git commit -m "feat: add IconButton.qml — Shape+PathSvg reusable button"
```

---

### Task 4: Create PlaylistEngine (C++)

**Files:**
- Create: `src/client/PlaylistEngine.h`
- Create: `src/client/PlaylistEngine.cpp`

- [ ] **Step 1: Write PlaylistEngine.h**

```cpp
#pragma once

#include <QObject>
#include <QStringList>

/// Playlist engine — folder traversal + playlist navigation.
/// No Qt UI dependency beyond QObject for QML property binding.
class PlaylistEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList files READ files NOTIFY filesChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(int count READ count NOTIFY filesChanged)
public:
    explicit PlaylistEngine(QObject* parent = nullptr);

    /// Scan a folder for video files. Returns number of files found.
    Q_INVOKABLE int scanFolder(const QString& path, int depth = 3);

    /// Navigate: returns the new current file path or empty if at boundary.
    Q_INVOKABLE QString next();
    Q_INVOKABLE QString previous();

    /// Accessors for QML.
    QStringList files() const { return files_; }
    int currentIndex() const { return currentIndex_; }
    QString currentFile() const;
    int count() const { return files_.size(); }

signals:
    void filesChanged();
    void currentIndexChanged();
    void currentFileChanged();

private:
    void scanDir(const QString& path, int remainingDepth);

    QStringList files_;
    int currentIndex_ = -1;
    QString rootPath_;
};
```

- [ ] **Step 2: Write PlaylistEngine.cpp**

```cpp
#include "PlaylistEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

// Common video extensions for filtering.
static const QSet<QString> kExtensions = {
    "mp4", "mkv", "webm", "avi", "mov", "ts", "m2ts", "flv", "wmv",
    "mpg", "mpeg", "m4v", "ogv", "divx", "webp"
};

PlaylistEngine::PlaylistEngine(QObject* parent) : QObject(parent) {}

int PlaylistEngine::scanFolder(const QString& path, int depth) {
    files_.clear();
    currentIndex_ = -1;
    rootPath_ = path;

    QFileInfo fi(path);
    if (fi.isFile()) {
        // Single file mode — just add it.
        files_.append(fi.absoluteFilePath());
        currentIndex_ = 0;
    } else if (fi.isDir()) {
        scanDir(path, depth);
        std::sort(files_.begin(), files_.end());
        if (!files_.isEmpty()) currentIndex_ = 0;
    }

    emit filesChanged();
    if (!files_.isEmpty()) {
        emit currentIndexChanged();
        emit currentFileChanged();
    }
    return files_.size();
}

void PlaylistEngine::scanDir(const QString& path, int remainingDepth) {
    if (remainingDepth <= 0) return;

    QDir dir(path);
    auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                     QDir::Name | QDir::DirsFirst);

    for (const auto& info : entries) {
        if (info.isDir()) {
            scanDir(info.absoluteFilePath(), remainingDepth - 1);
        } else if (info.isFile()) {
            QString ext = info.suffix().toLower();
            if (kExtensions.contains(ext)) {
                files_.append(info.absoluteFilePath());
            }
        }
    }
}

QString PlaylistEngine::currentFile() const {
    if (currentIndex_ < 0 || currentIndex_ >= files_.size())
        return {};
    return files_[currentIndex_];
}

QString PlaylistEngine::next() {
    if (files_.isEmpty()) return {};
    currentIndex_ = (currentIndex_ + 1) % files_.size();
    emit currentIndexChanged();
    emit currentFileChanged();
    return files_[currentIndex_];
}

QString PlaylistEngine::previous() {
    if (files_.isEmpty()) return {};
    currentIndex_ = (currentIndex_ - 1 + files_.size()) % files_.size();
    emit currentIndexChanged();
    emit currentFileChanged();
    return files_[currentIndex_];
}
```

- [ ] **Step 3: Commit**

```bash
git add src/client/PlaylistEngine.h src/client/PlaylistEngine.cpp
git commit -m "feat: add PlaylistEngine — folder scan + playlist navigation"
```

---

### Task 5: Rewrite main.cpp (Qt Quick client entry)

**Files:**
- Modify: `src/client/main.cpp` — full rewrite

This is the largest single file. The new `main.cpp` merges what was previously in `src/client_quick/main.cpp` with the CLI argument parsing from the old `src/client/main.cpp`, plus new features from the spec:
- Smart path detection (directory → playlist mode, file → single video mode)
- `--depth N` CLI flag
- Keyboard shortcuts (Space, Escape, Left/Right, Up/Down, F, S, P, N, B)
- IQmlEngine registration for `IconPaths` singleton

- [ ] **Step 1: Write new src/client/main.cpp**

```cpp
/// VSR Player — Qt Quick client (single wl_surface overlay UI).
/// Uses Qt's Vulkan resources (QRhi) for zero-copy CUDA-Vulkan interop.
/// QML overlays render on the same wl_surface as Vulkan video.

#include <cstdio>
#include <cstring>
#include <memory>

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>
#include <QKeyEvent>
#include <QQmlEngine>

#include "api/Player.h"
#include "PlayerCore.h"
#include "PlaylistEngine.h"

namespace vsr {

// ── Controller: QML bridge ──────────────────────────────────────────────

class QuickController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(int64_t currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString videoInfo READ videoInfo NOTIFY videoInfoChanged)
    Q_PROPERTY(bool vsrActive READ vsrActive NOTIFY vsrActiveChanged)
    Q_PROPERTY(bool hwDecoding READ hwDecoding NOTIFY hwDecodingChanged)
public:
    bool playing() const { return playing_; }
    int64_t currentTime() const { return currentTime_; }
    int64_t duration() const { return duration_; }
    QString videoInfo() const { return videoInfo_; }
    bool vsrActive() const { return vsrActive_; }
    bool hwDecoding() const { return hwDecoding_; }

    void setPlayer(Player* p) { player_ = p; }
    Player* player() const { return player_; }

public slots:
    void togglePlayPause() {
        if (!player_) return;
        playing_ = !playing_;
        emit playingChanged();
        player_->send_command({playing_ ? PlayerCommand::PLAY : PlayerCommand::PAUSE});
    }
    void play() {
        if (!player_ || playing_) return;
        playing_ = true; emit playingChanged();
        player_->send_command({PlayerCommand::PLAY});
    }
    void pause() {
        if (!player_ || !playing_) return;
        playing_ = false; emit playingChanged();
        player_->send_command({PlayerCommand::PAUSE});
    }
    void stop() {
        if (!player_) return;
        playing_ = false; emit playingChanged();
        player_->send_command({PlayerCommand::STOP});
    }
    void seek(int64_t ms) {
        if (!player_) return;
        player_->send_command({PlayerCommand::SEEK, "", ms});
    }
    void setVolume(double vol) {
        if (!player_) return;
        player_->send_command({PlayerCommand::SET_VOLUME, "", 0, vol});
    }
    void setQuality(int q) {
        if (!player_) return;
        player_->send_command({PlayerCommand::SET_QUALITY, "",
                               0, 0.0, 0.0, {}, static_cast<Quality>(q)});
    }
    void toggleMute() {
        if (!player_) return;
        muted_ = !muted_;
        player_->send_command({PlayerCommand::SET_MUTE, "", 0, 0.0, 0.0, {{"mute", muted_ ? "1" : "0"}}});
    }
    void screenshot() {
        if (player_) player_->send_command({PlayerCommand::CAPTURE_FRAME});
    }

    // Called from event callback (via QueuedConnection)
    void updateState(bool p) { if (playing_ != p) { playing_ = p; emit playingChanged(); } }
    void updateTime(int64_t t, int64_t d) {
        if (currentTime_ != t || duration_ != d) {
            currentTime_ = t; duration_ = d;
            emit currentTimeChanged();
            if (d != duration_) emit durationChanged();
        }
    }
    void updateVideoInfo(const QString& info) { videoInfo_ = info; emit videoInfoChanged(); }
    void updateVsrActive(bool a) { vsrActive_ = a; emit vsrActiveChanged(); }
    void updateHwDecoding(bool h) { hwDecoding_ = h; emit hwDecodingChanged(); }

signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void videoInfoChanged();
    void vsrActiveChanged();
    void hwDecodingChanged();

private:
    Player* player_ = nullptr;
    bool playing_ = false;
    bool muted_ = false;
    int64_t currentTime_ = 0;
    int64_t duration_ = 0;
    QString videoInfo_;
    bool vsrActive_ = false;
    bool hwDecoding_ = false;
};

// ── Key filter: keyboard shortcuts ──────────────────────────────────────

class KeyFilter : public QObject {
    Q_OBJECT
public:
    KeyFilter(QuickController* c, PlaylistEngine* pl, QObject* p)
        : QObject(p), ctrl(c), playlist(pl) {}

    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() != QEvent::KeyPress) return false;
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->isAutoRepeat()) return false;

        switch (ke->key()) {
        case Qt::Key_Space:
            ctrl->togglePlayPause(); return true;
        case Qt::Key_Escape:
            ctrl->stop(); return true;
        case Qt::Key_Left:
            ctrl->seek(ke->modifiers() & Qt::ShiftModifier ? -10000 : -5000);
            return true;
        case Qt::Key_Right:
            ctrl->seek(ke->modifiers() & Qt::ShiftModifier ? 10000 : 5000);
            return true;
        case Qt::Key_Up:
            ctrl->setVolume(0.05); return true;  // volume up (relative)
        case Qt::Key_Down:
            ctrl->setVolume(-0.05); return true;  // volume down (relative)
        case Qt::Key_F:
            if (auto* w = qobject_cast<QQuickWindow*>(parent()))
                w->setVisibility(w->visibility() == QWindow::FullScreen
                    ? QWindow::Windowed : QWindow::FullScreen);
            return true;
        case Qt::Key_S:
            ctrl->screenshot(); return true;
        case Qt::Key_P:
            emit togglePlaylist(); return true;
        case Qt::Key_N: {
            QString f = playlist->next();
            if (!f.isEmpty()) ctrl->player()->send_command({PlayerCommand::LOAD_FILE, f.toStdString()});
            return true;
        }
        case Qt::Key_B: {
            QString f = playlist->previous();
            if (!f.isEmpty()) ctrl->player()->send_command({PlayerCommand::LOAD_FILE, f.toStdString()});
            return true;
        }
        default: break;
        }
        return false;
    }

signals:
    void togglePlaylist();

private:
    QuickController* ctrl;
    PlaylistEngine* playlist;
};

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
    // Force Vulkan RHI + external memory extensions for CUDA interop.
    qputenv("QT_VULKAN_DEVICE_EXTENSIONS",
            "VK_KHR_external_memory;VK_KHR_external_memory_fd");
    qputenv("QSG_RHI_BACKEND", "vulkan");

    // ── CLI parsing ─────────────────────────────────────────────────

    bool use_vsr = true, no_hwaccel = false;
    vsr::Quality quality = vsr::Quality::HIGH;
    int scanDepth = 3;
    QString file;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-vsr")) use_vsr = false;
        else if (!strcmp(argv[i], "--no-hwaccel")) no_hwaccel = true;
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) {
            const char* q = argv[++i];
            if (!strcmp(q, "LOW")) quality = vsr::Quality::LOW;
            else if (!strcmp(q, "MEDIUM")) quality = vsr::Quality::MEDIUM;
            else if (!strcmp(q, "HIGH")) quality = vsr::Quality::HIGH;
            else if (!strcmp(q, "ULTRA")) quality = vsr::Quality::ULTRA;
        } else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
            scanDepth = atoi(argv[++i]);
            if (scanDepth < 1) scanDepth = 1;
        } else if (argv[i][0] != '-') {
            file = QString::fromLocal8Bit(argv[i]);
        }
    }

    // ── Qt app + QQuickView ─────────────────────────────────────────

    QGuiApplication app(argc, argv);
    app.setApplicationName("VSR Player");

    // Register IconPaths singleton in QML.
    qmlRegisterSingletonType(QUrl::fromLocalFile(
        "/home/zmq/projects/vsr-player/src/client/IconPaths.qml"),
        "VSR.IconPaths", 1, 0, "IconPaths");

    QQuickView view;
    view.setTitle("VSR Player");
    view.setMinimumSize({800, 600});
    view.setColor(QColor(10, 10, 10));

    // ── Controller + PlaylistEngine ─────────────────────────────────

    vsr::QuickController ctrl;
    view.rootContext()->setContextProperty("controller", &ctrl);

    PlaylistEngine playlist;
    view.rootContext()->setContextProperty("playlist", &playlist);

    auto* keyFilter = new vsr::KeyFilter(&ctrl, &playlist, &view);
    view.installEventFilter(keyFilter);

    // ── Scan folder / load file ─────────────────────────────────────

    if (!file.isEmpty()) {
        QFileInfo fi(file);
        if (fi.isDir()) {
            playlist.scanFolder(file, scanDepth);
        }
        // If file is a single video, PlaylistEngine scanFolder handles
        // single-file mode internally (adds it as the only entry).
        // If it's a dir, scanFolder populates the list.
        if (!fi.isDir()) {
            playlist.scanFolder(file, scanDepth);  // single file → 1 entry
        }
    }

    // ── Player init via beforeRenderPassRecording ───────────────────

    std::unique_ptr<vsr::Player> player;
    VkRenderPass compatRp = VK_NULL_HANDLE;
    bool ready = false;
    bool initAttempted = false;

    QObject::connect(&view, &QQuickWindow::beforeRenderPassRecording, [&]() {
        auto* rif = view.rendererInterface();
        if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan) return;

        // Get command buffer
        void* r = rif->getResource(&view, QSGRendererInterface::CommandListResource);
        if (!r) return;
        VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(r);
        if (!cb) return;

        // Physical pixel dimensions (HiDPI-safe)
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
            VkPhysicalDevice pd = *static_cast<VkPhysicalDevice*>(r);

            r = rif->getResource(&view, QSGRendererInterface::CommandQueueResource);
            if (!r) return;
            VkQueue q = *static_cast<VkQueue*>(r);

            auto* vkdf = vi->deviceFunctions(dev);
            if (!vkdf) return;

            compatRp = vsr::makeRenderPass(dev, vkdf);
            if (!compatRp) {
                fprintf(stderr, "Failed to create compatible render pass\n");
                return;
            }

            player = vsr::CreatePlayer();
            auto* pc = dynamic_cast<vsr::PlayerCore*>(player.get());
            ctrl.setPlayer(player.get());

            // Event callback: marshal to main thread via QueuedConnection
            player->set_event_callback([&](const vsr::PlayerEvent& e) {
                QMetaObject::invokeMethod(qApp, [&, e] {
                    switch (e.type) {
                    case vsr::PlayerEvent::VIDEO_INFO:
                        ctrl.updateState(true);
                        ctrl.updateVideoInfo(
                            QString("%1×%2 → %3×%4 | %5 fps | %6")
                                .arg(e.in_width).arg(e.in_height)
                                .arg(e.out_width).arg(e.out_height)
                                .arg(e.fps, 0, 'f', 1)
                                .arg(e.hw_decoding ? "NVDEC" : "SW"));
                        ctrl.updateVsrActive(e.vsr_active);
                        ctrl.updateHwDecoding(e.hw_decoding);
                        player->send_command({vsr::PlayerCommand::PLAY});
                        break;
                    case vsr::PlayerEvent::STATE_CHANGED:
                        ctrl.updateState(e.state == vsr::PlaybackState::PLAYING);
                        break;
                    case vsr::PlayerEvent::POSITION_CHANGED:
                        ctrl.updateTime(e.time_ms, e.duration_ms);
                        break;
                    case vsr::PlayerEvent::ERROR:
                        fprintf(stderr, "Player error: %s\n", e.error_msg.c_str());
                        break;
                    default: break;
                    }
                }, Qt::QueuedConnection);
            });

            if (pc->initialize_external(vi->vkInstance(), pd, dev, q, 0,
                                        compatRp, use_vsr, quality, no_hwaccel)) {
                ready = true;

                // Start with the first playlist file or the CLI file
                QString firstFile = playlist.currentFile();
                if (firstFile.isEmpty() && !file.isEmpty()) firstFile = file;
                if (firstFile.isEmpty()) {
                    fprintf(stderr, "No video file to play\n");
                    return;
                }

                player->send_command({vsr::PlayerCommand::RESIZE, "",
                                      (int64_t)w, (double)h, 0.0});
                player->send_command({vsr::PlayerCommand::LOAD_FILE,
                                      firstFile.toStdString()});
            } else {
                fprintf(stderr, "PlayerCore::initialize_external() failed\n");
            }
        }

        if (!ready) return;

        auto* pc = dynamic_cast<vsr::PlayerCore*>(player.get());
        if (!pc) return;

        // Clear background to dark color
        {
            r = rif->getResource(&view, QSGRendererInterface::DeviceResource);
            VkDevice dev = *static_cast<VkDevice*>(r);
            r = rif->getResource(&view, QSGRendererInterface::VulkanInstanceResource);
            auto* vkdf = static_cast<QVulkanInstance*>(r)->deviceFunctions(dev);

            VkClearAttachment ca{};
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = 0;
            ca.clearValue.color.float32[0] = 0.04f;  // ~#0a0a0a
            ca.clearValue.color.float32[1] = 0.04f;
            ca.clearValue.color.float32[2] = 0.04f;
            ca.clearValue.color.float32[3] = 1.0f;

            VkClearRect cr{};
            cr.layerCount = 1;
            cr.rect.extent = {(uint32_t)w, (uint32_t)h};
            vkdf->vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }

        // Draw video frame (if available) — letterboxed, physical pixels
        pc->record_frame(cb, w, h);

        // Keep the render loop alive
        view.update();
    });

    // ── Load QML overlay ────────────────────────────────────────────

    view.setSource(QUrl::fromLocalFile(
        "/home/zmq/projects/vsr-player/src/client/overlay.qml"));
    view.show();

    int ret = app.exec();

    // Cleanup
    if (player) {
        player->send_command({vsr::PlayerCommand::QUIT});
        player->shutdown();
    }

    return ret;
}

#include "main.moc"
```

- [ ] **Step 2: Commit**

```bash
git add src/client/main.cpp
git commit -m "feat: rewrite main.cpp — Qt Quick entry with overlay UI, keyboard shortcuts, playlist"
```

---

### Task 6: Write overlay.qml (full YouTube-style overlay)

**Files:**
- Create: `src/client/overlay.qml`

This is the complete overlay scene: TopBar, ControlBar (11 controls), CenterPlayButton, SettingsMenu, PlaylistPanel — all with declarative animations and auto-hide behavior.

- [ ] **Step 1: Write overlay.qml**

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import VSR.IconPaths 1.0

Item {
    id: root
    anchors.fill: parent

    // ── State sourced from C++ QuickController ──────────────────────

    property bool playing: controller ? controller.playing : false

    Connections {
        target: controller
        function onPlayingChanged() { root.playing = controller.playing }
    }

    // ── Auto-hide logic ─────────────────────────────────────────────

    property bool overlaysVisible: true

    function showOverlays() {
        overlaysVisible = true
        idleTimer.restart()
    }

    // Track mouse globally for auto-show
    MouseArea {
        id: globalMouseArea
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: showOverlays()
    }

    Timer {
        id: idleTimer
        interval: 3000
        onTriggered: overlaysVisible = false
    }

    // ── Top Bar ─────────────────────────────────────────────────────

    Rectangle {
        id: topBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48

        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }

        opacity: overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Text {
            id: filenameText
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            color: "#e0e0e0"
            font.pixelSize: 14
            elide: Text.ElideRight
            text: controller && controller.videoInfo ? controller.videoInfo : "VSR Player"
        }
    }

    // ── Center Play Button ──────────────────────────────────────────

    Rectangle {
        id: centerPlayBtn
        width: 72; height: 72; radius: 36
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        color: centerMouse.containsMouse
               ? (centerMouse.pressed ? "#99aa2222" : "#88aa2222")
               : "#66aa2222"

        opacity: (!root.playing && overlaysVisible) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Behavior on color { ColorAnimation { duration: 100 } }

        // Play/Pause icon via Shape
        Shape {
            anchors.centerIn: parent
            width: 28; height: 28
            layer.enabled: true; layer.samples: 4
            ShapePath {
                fillColor: centerMouse.containsMouse ? "#ffffff" : "#e0e0e0"
                strokeColor: "transparent"
                PathSvg { path: root.playing ? IconPaths.pause : IconPaths.play }
            }
        }

        MouseArea {
            id: centerMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: controller.togglePlayPause()
        }
    }

    // ── Bottom Control Bar ──────────────────────────────────────────

    Rectangle {
        id: controlBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 56

        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#cc000000" }
        }

        opacity: overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Row {
            id: controlsRow
            anchors {
                left: parent.left; right: parent.right
                verticalCenter: parent.verticalCenter
                leftMargin: 12; rightMargin: 12
            }
            spacing: 4
            layoutDirection: Qt.LeftToRight

            // 1. Previous
            IconButton {
                iconPath: IconPaths.skipBack; iconSize: 22
                tooltipText: "上一个 (B)"
                onClicked: {
                    var f = playlist.previous()
                    if (f) controller.player().send_command(
                        { type: 0 /* LOAD_FILE */, arg: f })
                }
            }

            // 2. Play/Pause
            IconButton {
                iconPath: root.playing ? IconPaths.pause : IconPaths.play
                iconSize: 28
                tooltipText: root.playing ? "暂停 (Space)" : "播放 (Space)"
                onClicked: controller.togglePlayPause()
            }

            // 3. Next
            IconButton {
                iconPath: IconPaths.skipForward; iconSize: 22
                tooltipText: "下一个 (N)"
                onClicked: {
                    var f = playlist.next()
                    if (f) controller.player().send_command(
                        { type: 0 /* LOAD_FILE */, arg: f })
                }
            }

            // 4. Stop
            IconButton {
                iconPath: IconPaths.stop; iconSize: 20
                tooltipText: "停止 (Esc)"
                onClicked: controller.stop()
            }

            // 5. Progress bar (takes remaining space)
            Slider {
                id: progressSlider
                from: 0
                to: controller ? controller.duration : 0
                value: controller ? controller.currentTime : 0
                width: parent ? parent.width
                       - (8 * 12) - 200 : 200  // remaining space after other controls
                anchors.verticalCenter: parent.verticalCenter

                onMoved: controller.seek(value)

                background: Rectangle {
                    x: progressSlider.leftPadding
                    y: progressSlider.topPadding + progressSlider.availableHeight / 2 - 2
                    implicitWidth: 200; implicitHeight: 4
                    width: progressSlider.availableWidth; height: 4
                    radius: 2
                    color: "#44ffffff"

                    Rectangle {
                        width: progressSlider.visualPosition * parent.width
                        height: parent.height; radius: 2
                        color: "#e0e0e0"
                    }
                }

                handle: Rectangle {
                    x: progressSlider.leftPadding + progressSlider.visualPosition
                       * (progressSlider.availableWidth - width)
                    y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
                    implicitWidth: 14; implicitHeight: 14
                    radius: 7
                    color: progressSlider.pressed ? "#ffffff" : "#e0e0e0"
                    visible: overlaysVisible || progressSlider.pressed
                }
            }

            // 6. Time label
            Text {
                text: {
                    function fmt(ms) {
                        var s = Math.floor(ms / 1000)
                        var m = Math.floor(s / 60)
                        var ss = s % 60
                        return m + ":" + (ss < 10 ? "0" : "") + ss
                    }
                    return fmt(controller ? controller.currentTime : 0)
                        + " / " + fmt(controller ? controller.duration : 0)
                }
                color: "#e0e0e0"
                font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }

            // 7. Volume
            IconButton {
                iconPath: IconPaths.volumeHigh; iconSize: 22
                tooltipText: "音量 (↑↓)"
                onClicked: controller.toggleMute()
            }

            // 8. Quality badge
            Rectangle {
                width: 52; height: 24; radius: 4
                color: "#44ffffff"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: "HIGH"
                    color: "#e0e0e0"
                    font.pixelSize: 11; font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: settingsMenu.open()
                }
            }

            // 9. HW/SW badge
            Rectangle {
                width: 52; height: 24; radius: 4
                color: controller && controller.hwDecoding ? "#4433ff33" : "#44ff3333"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: controller && controller.hwDecoding ? "NVDEC" : "SW"
                    color: "#e0e0e0"
                    font.pixelSize: 11; font.bold: true
                }
            }

            // 10. Settings
            IconButton {
                iconPath: IconPaths.settings; iconSize: 20
                tooltipText: "设置"
                onClicked: settingsMenu.open()
            }

            // 11. Playlist toggle
            IconButton {
                iconPath: IconPaths.playlist; iconSize: 20
                tooltipText: "播放列表 (P)"
                onClicked: playlistPanel.toggle()
            }
        }
    }

    // ── Settings Menu ────────────────────────────────────────────────

    Popup {
        id: settingsMenu
        x: parent.width - width - 60
        y: parent.height - height - 70
        width: 180
        padding: 8

        background: Rectangle {
            color: "#d9111111"
            radius: 8
            layer.enabled: true
        }

        Column {
            spacing: 4

            // VSR quality radio group
            Repeater {
                model: [
                    { text: "VSR OFF", value: -1 },
                    { text: "LOW", value: 0 },
                    { text: "MEDIUM", value: 1 },
                    { text: "HIGH", value: 2 },
                    { text: "ULTRA", value: 3 }
                ]

                Rectangle {
                    width: 164; height: 32; radius: 4
                    color: mouseArea.containsMouse ? "#33ffffff" : "transparent"

                    Text {
                        anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                        text: modelData.text
                        color: "#e0e0e0"; font.pixelSize: 13
                    }

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (modelData.value === -1) {
                                // VSR OFF — handled via controller
                            } else {
                                controller.setQuality(modelData.value)
                            }
                            settingsMenu.close()
                        }
                    }
                }
            }

            Rectangle { width: 164; height: 1; color: "#22ffffff" }

            // Screenshot
            Rectangle {
                width: 164; height: 32; radius: 4
                color: screenMouse.containsMouse ? "#33ffffff" : "transparent"

                Text {
                    anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                    text: "📷 Screenshot (S)"
                    color: "#e0e0e0"; font.pixelSize: 13
                }

                MouseArea {
                    id: screenMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: { controller.screenshot(); settingsMenu.close() }
                }
            }
        }
    }

    // ── Playlist Panel ───────────────────────────────────────────────

    Rectangle {
        id: playlistPanel
        x: parent.width  // offscreen right
        y: 0
        width: 320
        height: parent.height
        color: "#d9000000"

        // Slide animation
        Behavior on x {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }

        function toggle() {
            x = (x > parent.width / 2) ? parent.width - width : parent.width
        }

        // Header
        Rectangle {
            id: playlistHeader
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 48
            color: "#22ffffff"

            Text {
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                text: "播放列表"
                color: "#e0e0e0"; font.pixelSize: 15; font.bold: true
            }

            Text {
                anchors { right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: playlist ? playlist.count + " files" : ""
                color: "#999"; font.pixelSize: 12
            }

            IconButton {
                id: closeBtn
                iconPath: IconPaths.close; iconSize: 18
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                onClicked: playlistPanel.toggle()
            }
        }

        // List
        ListView {
            id: listView
            anchors { left: parent.left; right: parent.right;
                      top: playlistHeader.bottom; bottom: parent.bottom }
            model: playlist ? playlist.files : []
            clip: true

            delegate: Rectangle {
                width: 320; height: 42
                color: index === playlist.currentIndex
                       ? "#11ffffff" : "transparent"

                Row {
                    anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                    spacing: 8

                    Text {
                        text: (index + 1) + "."
                        color: index === playlist.currentIndex ? "#e0e0e0" : "#888"
                        font.pixelSize: 13
                        width: 28; horizontalAlignment: Text.AlignRight
                    }

                    Text {
                        text: modelData.split('/').pop()
                        color: index === playlist.currentIndex ? "#ffffff" : "#999"
                        font.pixelSize: 13
                        width: 250; elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (controller && controller.player()) {
                            controller.player().send_command(
                                { type: 0 /* LOAD_FILE */, arg: modelData })
                        }
                    }
                }
            }
        }
    }

    // Dismiss playlist when clicking outside it
    MouseArea {
        anchors.fill: parent
        visible: playlistPanel.x < parent.width
        z: playlistPanel.z - 1
        onClicked: playlistPanel.toggle()
    }

    // ── Initial state ───────────────────────────────────────────────

    Component.onCompleted: showOverlays()
}
```

- [ ] **Step 2: Commit**

```bash
git add src/client/overlay.qml
git commit -m "feat: add overlay.qml — YouTube-style overlay with all UI components"
```

---

### Task 7: Update Makefile

**Files:**
- Modify: `Makefile` — replace Widgets build with Quick build, remove client_quick targets, single `vsr-player` target

- [ ] **Step 1: Read current Makefile to locate exact replacement points**

The Makefile needs these changes:
1. `PKGS`: `Qt6Widgets` → `Qt6Quick`
2. Remove `QUICK_BIN`, `QUICK_CLIENTDIR`, `QUICK_PKGS`, `QUICK_CXXFLAGS`, `QUICK_LDFLAGS` variables
3. Remove `MOC_SRC` (MainWindow, VulkanWidget, PlayPauseButton MOC)
4. Remove `CLIENT_OBJS` (MainWindow.o, VulkanWidget.o, PlayPauseButton.o)
5. Replace with new client objects: `src/client/main.o`, `src/client/PlaylistEngine.o`, plus MOC for new `main.cpp`
6. Remove MOC rules for old Widgets files
7. Remove MOC dependency lines for old objects
8. Add MOC rule for new main.cpp (includes `main.moc`)
9. Add compilation rule for `src/client/PlaylistEngine.cpp`
10. Change `BIN` dependency: new client objects instead of old CLIENT_OBJS + MOC_OBJS
11. Remove `$(QUICK_BIN)` target and related rules
12. Remove QUICK-specific compilation rules
13. Shader rules: change `CLIENTDIR` references that point to `src/client/shaders/` (unchanged — already correct, just stay)
14. CXXFLAGS: add `-Isrc/client` (already there), update `PKGS`

- [ ] **Step 2: Make the edits**

Edit 1: Replace `PKGS` line (line 9):
```
PKGS := Qt6Widgets vulkan libavcodec libavformat libavutil libswscale wayland-client portaudio-2.0
```
→
```
PKGS := Qt6Quick vulkan libavcodec libavformat libavutil libswscale wayland-client portaudio-2.0
```

Edit 2: Remove Quick-specific variables (lines 38-55):
```
# ── Qt Quick client ──────────────────────────────────────────────────

QUICK_BIN := $(BUILD_DIR)/vsr-player-quick
...
```
Delete this entire block.

Edit 3: Replace MOC_SRC (lines 70-72):
```
MOC_SRC := $(BUILD_DIR)/moc_MainWindow.cpp \
           $(BUILD_DIR)/moc_VulkanWidget.cpp \
           $(BUILD_DIR)/moc_PlayPauseButton.cpp
```
→
```
MOC_SRC := $(BUILD_DIR)/moc_main_client.cpp
```

Edit 4: Replace CLIENT_OBJS (lines 88-91):
```
CLIENT_OBJS := $(BUILD_DIR)/src/client/main.o \
               $(BUILD_DIR)/src/client/MainWindow.o \
               $(BUILD_DIR)/src/client/VulkanWidget.o \
               $(BUILD_DIR)/src/client/PlayPauseButton.o
```
→
```
CLIENT_OBJS := $(BUILD_DIR)/src/client/main.o \
               $(BUILD_DIR)/src/client/PlaylistEngine.o
```

Edit 5: Replace MOC rule block (lines 140-150):
```
$(BUILD_DIR)/moc_MainWindow.cpp: $(CLIENTDIR)/MainWindow.h | $(BUILD_DIR)
	@echo "  MOC   MainWindow"
	@$(MOC) $< -o $@

$(BUILD_DIR)/moc_VulkanWidget.cpp: $(CLIENTDIR)/VulkanWidget.h | $(BUILD_DIR)
	@echo "  MOC   VulkanWidget"
	@$(MOC) $< -o $@

$(BUILD_DIR)/moc_PlayPauseButton.cpp: $(CLIENTDIR)/PlayPauseButton.h | $(BUILD_DIR)
	@echo "  MOC   PlayPauseButton"
	@$(MOC) $< -o $@
```
→
```
$(BUILD_DIR)/moc_main_client.cpp: $(CLIENTDIR)/main.cpp | $(BUILD_DIR)
	@echo "  MOC   main"
	@$(MOC) -I/usr/include/qt6 -I/usr/include/qt6/QtCore $< -o $@
```

Edit 6: Replace MOC dependency lines (lines 161-162):
```
$(BUILD_DIR)/src/client/MainWindow.o: $(BUILD_DIR)/moc_MainWindow.cpp
$(BUILD_DIR)/src/client/PlayPauseButton.o: $(BUILD_DIR)/moc_PlayPauseButton.cpp
```
→
```
$(BUILD_DIR)/src/client/main.o: $(BUILD_DIR)/moc_main_client.cpp $(SHADERS)
```

Edit 7: Remove Quick rules (lines 194-208):
```
# ── Qt Quick client ───────────────────────────────────────────────────

$(BUILD_DIR)/main.moc: $(QUICK_CLIENTDIR)/main.cpp | $(BUILD_DIR)
...
```
Delete this entire block.

Edit 8: Add include path for client dir (already in CXXFLAGS on line 23 as `-Isrc/client`, keep).

Edit 9: Add `-I/usr/include/qt6/QtQuick` to CXXFLAGS for MOC of QQuickWindow signals.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "feat: update Makefile — Qt6Widgets→Qt6Quick, single vsr-player target, remove client_quick"
```

---

### Task 8: Build verification + manual test

**Files:**
- None

- [ ] **Step 1: Clean build**

Run: `make clean && make`
Expected: Successful link of `build/vsr-player` with `Qt6Quick` and no `Qt6Widgets` dependency.

- [ ] **Step 2: Verify no Widgets dependency**

Run: `ldd build/vsr-player | grep -i qt`
Expected: `libQt6Quick`, `libQt6Qml`, `libQt6Gui`, `libQt6Core` — **no** `libQt6Widgets`.

- [ ] **Step 3: Verify file structure**

Run: `ls src/client/`
Expected: `main.cpp overlay.qml IconButton.qml IconPaths.qml PlaylistEngine.h PlaylistEngine.cpp shaders/` — **no** Widgets files, **no** `MainWindow.*`, `VulkanWidget.*`, `PlayPauseButton.*`.

- [ ] **Step 4: Verify src/client_quick/ gone**

Run: `ls src/client_quick/ 2>&1`
Expected: "No such file or directory"

- [ ] **Step 5: Dry-run launch**

Run: `./build/vsr-player --help`
Note: we don't have a `--help` handler yet, but it should start and show the empty QQuickView window.

- [ ] **Step 6: Commit verified build**

```bash
git add -A && git commit -m "build: verified Qt Quick client builds, no Widgets dependency"
```
(Only if changes exist — this may be a no-op if all files already committed)

---

### Task 9: Final review — spec compliance + code quality

**Files:**
- Review all new/changed files

- [ ] **Step 1: Spec compliance check**

Go through each spec requirement and verify:
- [x] Qt Quick single wl_surface architecture
- [x] `beforeRenderPassRecording` Vulkan video injection
- [x] TopBar with filename
- [x] ControlBar with 11 controls (Previous, Play/Pause, Next, Stop, Progress, Time, Volume, Quality, HW/SW, Settings, Playlist)
- [x] SettingsMenu (Popup with VSR quality + Screenshot)
- [x] PlaylistPanel (slide-in from right, ListView)
- [x] CenterPlayButton (72×72, visible when paused + overlays shown)
- [x] Shape + PathSvg icons (no Canvas)
- [x] Behavior/Transition animations (no imperative JS animations)
- [x] Auto-hide (3s idle timer)
- [x] HiDPI: physical pixels for Vulkan viewport, logical for QML layout
- [x] Keyboard shortcuts (Space, Escape, Left/Right, Up/Down, F, S, P, N, B)
- [x] PlaylistEngine C++ (folder scan, next/previous)
- [x] CLI: `--depth N`, smart path detection
- [x] Core unchanged (PlayerCore, VulkanRenderer, VulkanContext)
- [x] Shaders unchanged

- [ ] **Step 2: Code quality review**

- [ ] No `Qt6Widgets` references remain
- [ ] No `src/client_quick` references remain
- [ ] No `vsr-player-quick` references remain
- [ ] MOC correctly configured for Q_OBJECT in main.cpp
- [ ] Singleton registration for IconPaths
- [ ] PlaylistEngine exposed as `rootContext` property
- [ ] Event callback marshal uses `QueuedConnection` (correct for cross-thread)
- [ ] `view.update()` called each frame for continuous rendering
- [ ] HiDPI: `w = size().width() * devicePixelRatio()` for Vulkan dimensions
- [ ] No Canvas usage in QML (all Shape+PathSvg)
- [ ] No imperative JS animations (all Behavior/Transition)

- [ ] **Step 3: Commit any fixes**

```bash
git add -A && git commit -m "review: spec compliance + code quality fixes"
```
