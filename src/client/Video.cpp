#include "Video.h"
#include "Log.h"
#include "MpvController.h"
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QCoreApplication>
#include <cstdio>
#include <chrono>

extern "C" {
void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
void vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
void vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
void vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
VkResult vkQueueWaitIdle(VkQueue);
}

static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static double g_video_t0 = 0;
// 渲染循环逐帧诊断，默认关闭（VSR_LOG_VIDEO=1 开启）——对应规划中
// 的 DBG 语义，以环境变量门控避免误开（调试期不需要改代码）。
static bool g_video_verbose = getenv("VSR_LOG_VIDEO") != nullptr;
#define VLOG(fmt, ...) do { \
    if (g_video_verbose) { \
        double t = now() - g_video_t0; \
        fprintf(stderr, "[V %8.1fms] " fmt "\n", t * 1000.0, ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while(0)

// ── CompositeRenderNode ──────────────────────────────────────────────────

class CompositeRenderNode : public QSGRenderNode {
public:
    QQuickWindow *win = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkDescriptorSet curDs = VK_NULL_HANDLE;
    VkImage srcImage = VK_NULL_HANDLE;
    int w = 0, h = 0;

    StateFlags changedStates() const override { return BlendState | StencilState; }

    void render(const RenderState *) override {
        if (!srcImage || !win) return;
        auto *rif = win->rendererInterface();
        if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan) return;
        void *r = rif->getResource(win, QSGRendererInterface::CommandListResource);
        if (!r) return;
        VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(r);

        qreal dpr = win->devicePixelRatio();
        int fbW = (int)(win->width() * dpr), fbH = (int)(win->height() * dpr);
        if (fbW <= 0 || fbH <= 0) return;
        VkViewport vp = { 0, 0, (float)fbW, (float)fbH, 0, 1 };
        VkRect2D sc = { {0, 0}, {(uint32_t)fbW, (uint32_t)fbH} };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);

        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image         = srcImage;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout,
                                0, 1, &curDs, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

    RenderingFlags flags() const override { return {}; }
    QRectF rect() const override { return QRectF(0, 0, w, h); }
};

// ── Video ────────────────────────────────────────────────────────────────

Video::Video(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
}

void Video::requestRender() {
    // GUI 线程（mpv update callback 经 QMetaObject::invokeMethod
    // QueuedConnection 驱动——Qt 官方跨线程通道，社区标准模式）。
    // 条件检查：mpv 有渲染请求（新帧/seek/reconfig）才投递——播放时
    // 每帧命中，暂停/静默不命中（零渲染、零空转）。load() 只读不消费
    // ——消费在 updatePaintNode（need_render 判定，见 vo_libmpv flip_page
    // 语义）。
    if (!mpv_ || !window())
        return;
    bool need = mpv_->update() > 0 || renderRequested_.load();
    if (!need)
        return;
    update();  // 标记 dirty → sync 时 updatePaintNode 被调用
    // 兜底投递：QWindow::requestUpdate 在 Wayland QPA 有 frame-callback
    // 等待时静默丢弃（qwaylandwindow.cpp），postEvent 直接入队绕过。
    if (auto *w = window())
        QCoreApplication::postEvent(w, new QEvent(QEvent::UpdateRequest));
}

Video::~Video() {
    // Device outlives the QML object (view destroyed before mpv/pl_vulkan teardown).
    if (!dev_) return;
    if (rtView_)   vkDestroyImageView(dev_, rtView_, nullptr);
    if (rtImage_)  vkDestroyImage(dev_, rtImage_, nullptr);
    if (rtMem_)    vkFreeMemory(dev_, rtMem_, nullptr);
    if (rtSampler_) vkDestroySampler(dev_, rtSampler_, nullptr);
    if (compDsLayout_) vkDestroyDescriptorSetLayout(dev_, compDsLayout_, nullptr);
    if (compDsPool_)   vkDestroyDescriptorPool(dev_, compDsPool_, nullptr);
    if (cmdPool_)  vkDestroyCommandPool(dev_, cmdPool_, nullptr);
}

void Video::setMpvController(MpvController *mpv) {
    mpv_ = mpv;
}

bool Video::initRenderTarget(VkDevice dev, VkPhysicalDevice pd, uint32_t qfi, VkQueue queue) {
    dev_ = dev; pd_ = pd; queue_ = queue;

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = qfi;
    vkCreateCommandPool(dev_, &cpci, nullptr, &cmdPool_);

    // One-shot resources: sampler, descriptor set layout, pool.
    // The image/view/descriptor-set contents are managed by ensureRenderTarget().
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &sci, nullptr, &rtSampler_);

    VkDescriptorSetLayoutBinding dslb = {
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        1, VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr, 0, 1, &dslb
    };
    vkCreateDescriptorSetLayout(dev_, &dslci, nullptr, &compDsLayout_);

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr, 0, 1, 1, &dps
    };
    vkCreateDescriptorPool(dev_, &dpci, nullptr, &compDsPool_);

    VkDescriptorSetAllocateInfo dsai = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr, compDsPool_, 1, &compDsLayout_
    };
    vkAllocateDescriptorSets(dev_, &dsai, &compDs_);

    return true;
}

bool Video::ensureRenderTarget(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (rtImage_ != VK_NULL_HANDLE && w == w_ && h == h_) return true;

    // Previous frame's GPU work is complete (vkQueueWaitIdle after each render).
    if (rtImage_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev_, rtView_, nullptr);
        vkDestroyImage(dev_, rtImage_, nullptr);
        vkFreeMemory(dev_, rtMem_, nullptr);
    }

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels     = 1; ici.arrayLayers = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev_, &ici, nullptr, &rtImage_);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev_, rtImage_, &mr);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(pd_, &memProps);
    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memTypeIdx = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = memTypeIdx;
    vkAllocateMemory(dev_, &mai, nullptr, &rtMem_);
    vkBindImageMemory(dev_, rtImage_, rtMem_, 0);

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = rtImage_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = ivci.subresourceRange.layerCount = 1;
    vkCreateImageView(dev_, &ivci, nullptr, &rtView_);

    // Update descriptor set contents in place — the set handle never changes,
    // so the scene-graph render node needs no awareness of this recreation.
    VkDescriptorImageInfo dii = {
        rtSampler_, rtView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                 compDs_, 0, 0, 1,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 &dii, nullptr, nullptr };
    vkUpdateDescriptorSets(dev_, 1, &wds, 0, nullptr);

    w_ = w; h_ = h;
    MLOG_INFO("render target %dx%d", w_, h_);
    return true;
}

void Video::setCompositeObjects(VkPipeline pipe, VkPipelineLayout layout) {
    pipe_ = pipe; pipeLayout_ = layout;
}

void Video::kickstart() {
    update();
}

// ── Scene graph ─────────────────────────────────────────────────────────

QSGNode *Video::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    static int fc = 0; fc++;
    if (g_video_t0 == 0.0) g_video_t0 = now();
    double t_upn = now();

    auto *node = static_cast<CompositeRenderNode*>(old);
    if (!node) {
        node = new CompositeRenderNode();
        node->win = window();
        node->dev = dev_;
        node->pipe = pipe_;
        node->pipeLayout = pipeLayout_;
    }
    if (auto *w = window()) {
        qreal dpr = w->devicePixelRatio();
        node->w = (int)(w->width() * dpr);
        node->h = (int)(w->height() * dpr);
    }

    // ── render target sizing (window framebuffer size) ────────────────
    // resize 节流：尺寸连续 RT_STABLE_FRAMES 帧未变才重建。拖动中
    // 每帧尺寸变化 → 只更新 pending（零重建）；停止后 ~3 帧重建一次。
    // 重建会触发 mpv reconfig（VSR 分辨率重配 + vsr_reconfigure），
    // 若每帧重建 → resize 拖动全程卡顿。
    if (node->w > 0 && node->h > 0) {
        if (node->w == pendingW_ && node->h == pendingH_) {
            stableCount_++;
            if (stableCount_ >= RT_STABLE_FRAMES)
                ensureRenderTarget(pendingW_, pendingH_);
        } else {
            if (node->w != pendingW_ || node->h != pendingH_)
                VLOG("rt resize pending %dx%d (was %dx%d)",
                     node->w, node->h, pendingW_, pendingH_);
            pendingW_ = node->w;
            pendingH_ = node->h;
            stableCount_ = 0;
        }
    }

    // ── mpv work ─────────────────────────────────────────────────────
    // 播放结束由事件线程的 MPV_EVENT_END_FILE 判定（见 main.cpp），
    // 此处只负责渲染。
    if (mpv_ && rtImage_ != VK_NULL_HANDLE) {
        double t_upd0 = now();
        uint64_t uf = mpv_->update();
        double t_upd1 = now();
        // Render also when the update callback fired without a new frame:
        // seek/reconfig leaves pending VO work (vo_libmpv flip_page waits
        // on next_frame) which render() must consume, or the core declares
        // the render context stuck and ends playback.
        bool need_render = uf > 0 || renderRequested_.exchange(false);
        if (need_render) {
            // 只统计真实新帧（uf>0）；renderRequested_ 触发的状态渲染
            // （seek/reconfig 消费 pending VO 工作）不计入 benchmark。
            if (uf > 0) {
                if (benchT0_.load() == 0.0) benchT0_.store(now());
                benchFrames_.fetch_add(1);
                if (frameRenderedCb_) frameRenderedCb_();   // 段内 rendered 计数
            }
            double t_rnd0 = now();
            mpv_->render(rtImage_, VK_FORMAT_R8G8B8A8_UNORM, w_, h_);
            double t_rnd1 = now();
            mpv_->reportSwap();
            double t_wait0 = now();
            vkQueueWaitIdle(queue_);
            double t_wait1 = now();
            VLOG("#%-5d upn up=%.0fus rnd=%.0fus wait=%.0fus tot=%.0fus",
                 fc, (t_upd1 - t_upd0) * 1e6, (t_rnd1 - t_rnd0) * 1e6,
                 (t_wait1 - t_wait0) * 1e6, (t_wait1 - t_upn) * 1e6);
        }
    }

    // ── pass to render node ──────────────────────────────────────────
    node->curDs = compDs_;
    node->srcImage = rtImage_;
    // dirty 由 onAboutToBlock（GUI 线程、事件处理完成后）条件设置——
    // 这里绝不无条件 update()/postEvent（那会制造渲染事件风暴抢占
    // 事件循环，实测 EVT 400-1350ms 饿死输入）。

    return node;
}
