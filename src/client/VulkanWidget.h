#pragma once

#include <QWidget>

#include "utils/VulkanRenderer.h"
#include "utils/InteropTexture.h"

namespace vsr {

/// QWidget embedding a Vulkan render surface (Wayland).
class VulkanWidget : public QWidget {
    Q_OBJECT

public:
    explicit VulkanWidget(QWidget* parent = nullptr);
    ~VulkanWidget() override;

    bool init_vulkan();

    /// Initialize pipelines after video dimensions are known.
    /// @param videoW, videoH  Native (pre-scale) video frame dimensions.
    /// @param scale  VSR scale factor (1 for 1:1, 2 for 2x, etc.)
    bool init_pipelines(int videoW, int videoH, int scale);

    /// Render a frame using the specified path.
    /// The caller must fill the relevant InteropTextures via CUDA D2D
    /// copies before calling this.
    bool render_frame(Path path);

    /// Explicitly release Vulkan resources (including InteropTextures)
    /// before the CUDA context is destroyed.
    void releaseRenderer();

    // InteropTexture accessors for CUDA D2D copies
    InteropTexture& rgbaInterop() { return renderer_.rgbaInterop(); }
    InteropTexture& yInterop()    { return renderer_.yInterop(); }
    InteropTexture& uvInterop()   { return renderer_.uvInterop(); }

    QPaintEngine* paintEngine() const override { return nullptr; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    VulkanRenderer renderer_;
    bool vulkan_ready_ = false;
};

}  // namespace vsr
