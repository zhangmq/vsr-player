#pragma once
#include <QQuickItem>
#include <vulkan/vulkan.h>
#include <atomic>
#include <functional>

class MpvController;

/// Single-threaded video renderer.
/// mpv work (update→render→reportSwap) happens in updatePaintNode on the main thread.
/// No locks, no threads — basic render loop only.
///
/// Render target (rtImage) is sized to the item's window framebuffer size
/// (width × dpr). Recreated on size change; the descriptor set handle stays
/// stable across recreations (contents updated via vkUpdateDescriptorSets).
class Video : public QQuickItem {
    Q_OBJECT
public:
    explicit Video(QQuickItem *parent = nullptr);
    ~Video() override;

    void setMpvController(MpvController *mpv);
    bool initRenderTarget(VkDevice dev, VkPhysicalDevice pd, uint32_t qfi, VkQueue queue);
    void setCompositeObjects(VkPipeline pipe, VkPipelineLayout layout);

    /// (Re)create render target for the given pixel size. Rebuilds
    /// image + view, then updates the descriptor set contents in place.
    /// Idempotent when size unchanged.
    bool ensureRenderTarget(int w, int h);

    VkImage image() const { return rtImage_; }
    VkDescriptorSetLayout descriptorSetLayout() const { return compDsLayout_; }
    int width() const { return w_; }
    int height() const { return h_; }

    std::atomic<double> lastCb_{0.0};
    /// Set by the update callback: mpv requests rendering (new frame or
    /// state change — seek/reconfig produce no new frame but still need
    /// render to consume pending VO work; see vo_libmpv flip_page).
    std::atomic<bool> renderRequested_{false};

    /// Benchmark counters (updated on the render/main thread, read by the
    /// event thread on END_FILE — hence atomic).
    std::atomic<int> benchFrames_{0};
    std::atomic<double> benchT0_{0.0};

    /// 渲染新帧回调（uf>0 分支，主线程）——viewModel 段内 rendered 计数。
    void setFrameRenderedCallback(std::function<void()> cb) { frameRenderedCb_ = std::move(cb); }

public slots:
    void kickstart();
    /// mpv update callback（核心线程）invokeMethod(QueuedConnection) 驱动
    /// 的渲染请求（社区标准模式，见 trin94/qtquick-mpv）。GUI 线程执行：
    /// 条件检查（need_render）→ update() + postEvent 投递渲染请求。
    void requestRender();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    MpvController   *mpv_ = nullptr;
    VkDevice         dev_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;

    VkImage          rtImage_ = VK_NULL_HANDLE;
    VkDeviceMemory   rtMem_ = VK_NULL_HANDLE;
    VkImageView      rtView_ = VK_NULL_HANDLE;
    VkSampler        rtSampler_ = VK_NULL_HANDLE;
    VkCommandPool    cmdPool_ = VK_NULL_HANDLE;
    int w_ = 0, h_ = 0;      // current render target pixel size

    VkPipeline       pipe_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compDsLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      compDsPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       compDs_ = VK_NULL_HANDLE;

    std::function<void()> frameRenderedCb_;

    // ── RT resize 节流（稳定计数）─────────────────────────────────
    // resize 拖动时每帧尺寸变化，直接 ensureRenderTarget 会每帧重建
    // rt + 触发 mpv reconfig（VSR 分辨率重配）→ 卡顿。改为：尺寸连续
    // RT_STABLE_FRAMES 帧未变才重建。拖动中零重建；停止后 ~30 帧重建
    //（0.5s@60fps / 1.25s@24fps）。保守节流：全屏/窗口切换的尺寸连续
    // 变化会重置 stableCount，重建自然延后到切换完成后（modeset 与
    // render target 重建的 GPU 操作错开）。Xid 109 竞态的完整修复链
    // 见 memory nvidia-xid109（uninit 同步 + hold flush + destroy
    // 流级同步 + RENDER_HALT_MS 渲染暂停）。
    static constexpr int RT_STABLE_FRAMES = 30;
    int pendingW_ = 0, pendingH_ = 0;
    int stableCount_ = 0;
    /// 尺寸变化后的渲染暂停窗口（ms）：全屏/窗口切换触发 render target
    /// 重建 + VSR 引擎重建（auto scale 变化），SDK 的 DestroyEffect 要求
    /// 全部 CUDA 流空闲——渲染循环每帧 map（stream 0）使它永不空闲 →
    /// 卡死（Xid 109 场景，core 实测卡在 NvNGXFeatureHelper::ReleaseBuffers）。
    /// 暂停窗口内跳过 mpv render：stream 0 排空、GPU 静止，重建完成后再
    /// 恢复渲染。与 RT_STABLE_FRAMES 节流同一思路（重建避开 GPU 忙期）。
    static constexpr double RENDER_HALT_MS = 2500.0;
    double renderHaltUntil_ = 0.0;
};
