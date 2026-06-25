/// Qt Quick overlay experiment — full interaction port from Qt Widgets.
/// Build: moc quick_player.cpp -o build/tests/quick_player.moc && g++ ...

#include <cstdio>
#include <cmath>

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>
#include <QKeyEvent>
#include <QTimer>

// ═══════════════════════════════════════════════════════════════════════
// PlayerController — bridges Qt Quick ↔ C++ player state
// ═══════════════════════════════════════════════════════════════════════

class PlayerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(QString state READ state WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QString timeText READ timeText NOTIFY timeChanged)
    Q_PROPERTY(QString videoInfo READ videoInfo NOTIFY videoInfoChanged)

public:
    explicit PlayerController(QObject* parent = nullptr) : QObject(parent) {}

    bool playing() const { return playing_; }
    QString state() const { return state_; }
    QString timeText() const { return timeText_; }
    QString videoInfo() const { return videoInfo_; }

    void setPlaying(bool p) {
        if (playing_ != p) {
            playing_ = p;
            emit playingChanged();
            setState(p ? "playing" : "paused");
        }
    }

    void setState(const QString& s) {
        if (state_ != s) {
            state_ = s;
            emit stateChanged();
        }
    }

    void setTimeText(const QString& t) {
        if (timeText_ != t) {
            timeText_ = t;
            emit timeChanged();
        }
    }

    void setVideoInfo(const QString& info) {
        if (videoInfo_ != info) {
            videoInfo_ = info;
            emit videoInfoChanged();
        }
    }

public slots:
    void togglePlayPause() {
        setPlaying(!playing_);
        fprintf(stderr, "  [Controller] %s\n", playing_ ? "PLAYING" : "PAUSED");
    }

signals:
    void playingChanged();
    void stateChanged();
    void timeChanged();
    void videoInfoChanged();

private:
    bool playing_ = false;
    QString state_ = "stopped";
    QString timeText_ = "00:00 / 00:00";
    QString videoInfo_ = "";
};

// ═══════════════════════════════════════════════════════════════════════
// Event filter — keyboard (Space=toggle, S=screenshot)
// ═══════════════════════════════════════════════════════════════════════

class KeyFilter : public QObject {
    Q_OBJECT
public:
    KeyFilter(PlayerController* ctrl, QQuickView* view)
        : QObject(view), ctrl_(ctrl), view_(view) {}

    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (!ke->isAutoRepeat()) {
                if (ke->key() == Qt::Key_Space) {
                    ctrl_->togglePlayPause();
                    return true;
                }
                if (ke->key() == Qt::Key_S) {
                    fprintf(stderr, "  [Key] Screenshot requested\n");
                    return true;
                }
            }
        }
        return false;
    }

private:
    PlayerController* ctrl_;
    QQuickView* view_;
};

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    qputenv("QSG_RHI_BACKEND", "vulkan");

    QGuiApplication app(argc, argv);
    app.setApplicationName("VSR Player Quick");

    QQuickView view;
    view.setTitle("VSR Player — Qt Quick overlay experiment");
    view.setMinimumSize(QSize(800, 600));
    view.setColor(QColor(20, 20, 20));

    // ── Player controller ──────────────────────────────────────────

    PlayerController controller;
    view.rootContext()->setContextProperty("controller", &controller);

    // ── Keyboard ───────────────────────────────────────────────────

    KeyFilter keyFilter(&controller, &view);
    view.installEventFilter(&keyFilter);

    // ── Vulkan background rendering ────────────────────────────────
    // In production, this renders decoded+VSR-processed video frames.
    // For the experiment: test pattern (dark green bg + colored bottom bar).

    QObject::connect(&view, &QQuickWindow::beforeRenderPassRecording,
                     [&view]() {
        auto* rif = view.rendererInterface();
        if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan)
            return;

        void* r = rif->getResource(&view, QSGRendererInterface::VulkanInstanceResource);
        if (!r) return;
        auto* vkInst = static_cast<QVulkanInstance*>(r);

        r = rif->getResource(&view, QSGRendererInterface::DeviceResource);
        if (!r) return;
        VkDevice dev = *static_cast<VkDevice*>(r);

        auto* vkdf = vkInst->deviceFunctions(dev);
        if (!vkdf) return;

        r = rif->getResource(&view, QSGRendererInterface::CommandListResource);
        if (!r) return;
        VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(r);
        if (cb == VK_NULL_HANDLE) return;

        int w = (int)(view.size().width() * view.devicePixelRatio());
        int h = (int)(view.size().height() * view.devicePixelRatio());
        if (w <= 0 || h <= 0) return;

        // Background: dark green gradient-like via two clears
        {
            VkClearAttachment att{};
            att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            att.clearValue.color.float32[0] = 0.10f;
            att.clearValue.color.float32[1] = 0.22f;
            att.clearValue.color.float32[2] = 0.10f;
            att.clearValue.color.float32[3] = 1.0f;

            VkClearRect rect{};
            rect.layerCount = 1;
            rect.rect.extent = {(uint32_t)w, (uint32_t)h};
            vkdf->vkCmdClearAttachments(cb, 1, &att, 1, &rect);
        }

        // Test pattern at bottom (simulating video controls area)
        int barH = (int)(100 * view.devicePixelRatio());
        int y0 = h - barH;

        VkClearAttachment att{};
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.colorAttachment = 0;
        att.clearValue.color.float32[0] = 0.06f;
        att.clearValue.color.float32[1] = 0.10f;
        att.clearValue.color.float32[2] = 0.22f;
        att.clearValue.color.float32[3] = 1.0f;

        VkClearRect rect{};
        rect.layerCount = 1;
        rect.rect.offset = {0, (int32_t)y0};
        rect.rect.extent = {(uint32_t)w, (uint32_t)barH};
        vkdf->vkCmdClearAttachments(cb, 1, &att, 1, &rect);

        static int n = 0;
        if (++n <= 2)
            fprintf(stderr, "  Vulkan bg frame %d OK (%dx%d)\n", n, w, h);
    });

    // ── Load QML overlay ───────────────────────────────────────────

    view.setSource(QUrl::fromLocalFile(
        "/home/zmq/projects/vsr-player/tests/test_qtquick_overlay/player_overlay.qml"));
    if (view.status() != QQuickView::Ready) {
        fprintf(stderr, "QML load failed:\n");
        for (auto& e : view.errors())
            fprintf(stderr, "  %s\n", qPrintable(e.toString()));
        return 1;
    }

    view.show();

    auto* rif = view.rendererInterface();
    fprintf(stderr, "=== VSR Player Quick — Overlay Experiment ===\n");
    fprintf(stderr, "RHI: %s\n",
            rif->graphicsApi() == QSGRendererInterface::Vulkan ? "Vulkan ✓" : "other");
    fprintf(stderr, "Controls:\n");
    fprintf(stderr, "  Mouse move  → show overlays\n");
    fprintf(stderr, "  Click circle → toggle play/pause\n");
    fprintf(stderr, "  Space       → toggle play/pause\n");
    fprintf(stderr, "  S           → screenshot\n");
    fprintf(stderr, "Overlays fade out after 800ms idle.\n");

    return app.exec();
}

#include "quick_player.moc"
