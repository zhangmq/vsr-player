/// Qt Quick + Vulkan overlay — semi-transparent UI on Vulkan pattern.

#include <cstdio>
#include <cmath>

#include <QGuiApplication>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>

int main(int argc, char* argv[]) {
    qputenv("QSG_RHI_BACKEND", "vulkan");

    QGuiApplication app(argc, argv);

    QQuickView view;
    view.setTitle("Qt Quick + Vulkan — pattern + semi-transparent overlay");
    view.setMinimumSize(QSize(800, 600));
    view.setColor(QColor(20, 20, 20));

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

        // Clear full screen to dark green
        {
            VkClearAttachment att{};
            att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            att.clearValue.color.float32[0] = 0.12f;
            att.clearValue.color.float32[1] = 0.28f;
            att.clearValue.color.float32[2] = 0.12f;
            att.clearValue.color.float32[3] = 1.0f;

            VkClearRect rect{};
            rect.layerCount = 1;
            rect.rect.extent = {(uint32_t)w, (uint32_t)h};

            vkdf->vkCmdClearAttachments(cb, 1, &att, 1, &rect);
        }

        // Draw a test pattern with colored stripes at the bottom
        // This simulates "Vulkan-rendered UI" under the QML transparent overlay
        int barH = (int)(100 * view.devicePixelRatio());
        int y0 = h - barH;
        if (y0 < 0) y0 = 0;

        // Colored horizontal stripes in the bottom bar area
        struct { float r, g, b; } colors[] = {
            {0.05f, 0.08f, 0.20f}, // dark blue
            {0.08f, 0.06f, 0.18f}, // dark purple
            {0.05f, 0.08f, 0.20f}, // dark blue
            {0.06f, 0.09f, 0.19f}, // blue-grey
            {0.04f, 0.07f, 0.19f}, // darker blue
        };
        int nStripes = sizeof(colors) / sizeof(colors[0]);
        int stripeH = barH / nStripes;

        for (int i = 0; i < nStripes; i++) {
            VkClearAttachment att{};
            att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            att.clearValue.color.float32[0] = colors[i].r;
            att.clearValue.color.float32[1] = colors[i].g;
            att.clearValue.color.float32[2] = colors[i].b;
            att.clearValue.color.float32[3] = 1.0f;

            VkClearRect rect{};
            rect.layerCount = 1;
            rect.rect.offset = {(int32_t)0, (int32_t)(y0 + i * stripeH)};
            rect.rect.extent = {(uint32_t)w, (uint32_t)(stripeH + 1)};

            vkdf->vkCmdClearAttachments(cb, 1, &att, 1, &rect);
        }

        // Colored vertical blocks on top of the stripes (simulating "controls")
        int blockW = (int)(60 * view.devicePixelRatio());
        int blockH = (int)(36 * view.devicePixelRatio());
        int blockY = y0 + (barH - blockH) / 2;

        struct { float r, g, b; int x; } blocks[] = {
            {0.15f, 0.30f, 0.15f, (int)(20 * view.devicePixelRatio())},            // green
            {0.15f, 0.15f, 0.30f, (int)(100 * view.devicePixelRatio())},           // blue
            {0.30f, 0.15f, 0.15f, (int)(180 * view.devicePixelRatio())},           // red
            {0.25f, 0.25f, 0.10f, (int)(260 * view.devicePixelRatio())},           // yellow
        };

        for (auto& blk : blocks) {
            VkClearAttachment att{};
            att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            att.clearValue.color.float32[0] = blk.r;
            att.clearValue.color.float32[1] = blk.g;
            att.clearValue.color.float32[2] = blk.b;
            att.clearValue.color.float32[3] = 1.0f;

            VkClearRect rect{};
            rect.layerCount = 1;
            rect.rect.offset = {(int32_t)blk.x, (int32_t)blockY};
            rect.rect.extent = {(uint32_t)blockW, (uint32_t)blockH};

            vkdf->vkCmdClearAttachments(cb, 1, &att, 1, &rect);
        }

        static int n = 0;
        if (++n <= 2)
            fprintf(stderr, "  Vulkan pattern frame %d OK (%dx%d, bar=%d)\n",
                    n, w, h, barH);
    });

    view.setSource(QUrl::fromLocalFile(
        "/home/zmq/projects/vsr-player/tests/test_qtquick_overlay/overlay.qml"));
    if (view.status() != QQuickView::Ready) {
        fprintf(stderr, "QML load failed\n");
        for (auto& e : view.errors())
            fprintf(stderr, "  %s\n", qPrintable(e.toString()));
        return 1;
    }

    view.show();

    auto* rif = view.rendererInterface();
    fprintf(stderr, "=== Qt Quick + Vulkan Test Pattern ===\n");
    fprintf(stderr, "RHI: %s\n",
            rif->graphicsApi() == QSGRendererInterface::Vulkan ? "Vulkan ✓" : "other");
    fprintf(stderr, "Vulkan: dark green bg + colored bottom bar + colored blocks\n");
    fprintf(stderr, "QML: semi-transparent circle (center) + overlay controls (bottom)\n");
    fprintf(stderr, "Check: can you see BOTH Vulkan pattern AND QML overlays?\n");

    return app.exec();
}
