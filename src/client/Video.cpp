#include "Video.h"
#include "Log.h"
#include "MpvController.h"
#include "StereoController.h"
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QCoreApplication>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>

extern "C" {
void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
void vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
void vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
void vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
void vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
VkResult vkQueueWaitIdle(VkQueue);
VkResult vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*);
VkResult vkEndCommandBuffer(VkCommandBuffer);
VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
void vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
VkResult vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
void vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*);
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
    Video *owner = nullptr;          ///< 渲染阶段从 owner 取当前 pipeline（见下）
    VkDevice dev = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;          ///< 兜底（owner 为空时）
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
        // 与渲染目标一致：宽度向上取偶（奇数宽无法被左右眼均分）。
        fbW = Video::compositeWidth(fbW);
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

        // 运行时取当前 pipeline：切换 SDR↔HDR10 时 composite pipeline 会被
        // 重建，缓存句柄在"没有 updatePaintNode 的帧"里会变成已销毁对象。
        const VkPipeline pipeNow = owner ? owner->compositePipeline() : pipe;
        const VkPipelineLayout layoutNow = owner ? owner->compositePipelineLayout() : pipeLayout;
        if (!pipeNow || !layoutNow) return;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeNow);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutNow,
                                0, 1, &curDs, 0, nullptr);
        // 3D 合成参数：每帧取一次（布局/眼序/缩放可运行期切换）。
        const StereoCompositeParams sp = owner
            ? owner->stereoParams(fbW, fbH) : StereoCompositeParams{};
        vkCmdPushConstants(cb, layoutNow, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(sp), &sp);
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
    if (rtImage_ != VK_NULL_HANDLE && w == w_ && h == h_ &&
        renderFormat_ == fmt_) return true;

    // 销毁前无条件排空 GPU：原实现依赖"上一帧 need_render 时执行过
    // vkQueueWaitIdle"——但 need_render=false 的帧不 waitIdle，而场景图
    // 绘制命令（引用 rtImage）每帧提交——销毁旧 rtImage 时可能有在途
    // 命令引用它 → Vulkan UAF → GPU hang（Xid 109 根因，2026-08-05
    // 定位，4/4 压测验证）。vkDestroyImage 要求该 image 不被队列引用。
    vkQueueWaitIdle(queue_);

    if (rtImage_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev_, rtView_, nullptr);
        vkDestroyImage(dev_, rtImage_, nullptr);
        vkFreeMemory(dev_, rtMem_, nullptr);
    }

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = renderFormat_;   // SDR=R8G8B8A8，HDR10=A2B10G10R10
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

    // 首帧前清黑：新分配 image 内容未定义——引擎加载（rife/vsr init
    // 秒级）阻塞 mpv 首帧输出期间，场景图已采样 rtImage（VO 无帧），
    // 未初始化 GPU 内存上屏 = 噪点（冷启动/重启续播实证 2026-08-12，
    // 480p 明显 720p 无感——引擎加载窗口差异）。清黑后该窗口为黑屏。
    // mpv 首帧渲染以 UNDEFINED 转换（丢弃内容）重绘全帧，无冲突。
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cmdPool_; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(dev_, &cbai, &cb) == VK_SUCCESS) {
        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (vkBeginCommandBuffer(cb, &cbbi) == VK_SUCCESS) {
            VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b1.image = rtImage_;
            b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b1.subresourceRange.levelCount = b1.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b1);
            VkClearColorValue cc = {{0, 0, 0, 1}};
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, rtImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &cc, 1, &range);
            VkImageMemoryBarrier b2 = b1;
            b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b2);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue_);
        }
        vkFreeCommandBuffers(dev_, cmdPool_, 1, &cb);
    }

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = rtImage_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = renderFormat_;
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

    w_ = w; h_ = h; fmt_ = renderFormat_;
    MLOG_INFO("render target %dx%d fmt=%d", w_, h_, int(fmt_));
    return true;
}

StereoCompositeParams Video::stereoParams(int outW, int outH) const {
    StereoCompositeParams p;
    p.outAspect = outH > 0 ? float(outW) / float(outH) : 1.0f;
    p.outW = outW;
    p.outH = outH;
    p.srcAspect = p.outAspect;
    if (srcAspectFn_) {
        const double a = srcAspectFn_();
        if (a > 0.01 && a < 100.0)
            p.srcAspect = float(a);
    }
    if (srcSizeFn_) {
        const QSize s = srcSizeFn_();
        if (s.isValid()) {
            p.srcW = s.width();
            p.srcH = s.height();
        }
    }
    if (!stereo_ || !stereo_->active())
        return p;
    p.active = 1;
    // 2D→3D 暂以“2D 复制到双眼”占位（无深度）；3D 片源走 SBS/上下。
    p.layout = stereo_->layout();
    p.swap = stereo_->swap() ? 1 : 0;
    p.halfMode = stereo_->half() ? 1 : 0;

    // 参数变化时打一行（含每眼实际像素尺寸）：3D 几何问题一律先看这行，
    // 与 composite.frag 用同一套整数算式（splitX = outW/2）。
    if (!loggedMapValid_ || std::memcmp(&loggedMap_, &p, sizeof(p)) != 0) {
        loggedMap_ = p;
        loggedMapValid_ = true;
        const int splitX = std::max(p.outW / 2, 1);
        const float eyeW = float(splitX), eyeH = float(p.outH);
        float imgW = float(p.srcW), imgH = float(p.srcH);
        if (p.halfMode == 0) {
            if (p.layout == 1) imgH *= 0.5f;
            else               imgW *= 0.5f;
        }
        float sc = 1.0f;
        if (imgW >= 1.0f && imgH >= 1.0f)
            sc = std::min(eyeW / imgW, eyeH / imgH);
        MLOG_INFO("stereo map: out=%dx%d splitX=%d src=%dx%d layout=%s half=%d swap=%d"
                  " -> eye=%.1fx%.1f content=%.1fx%.1f",
                  p.outW, p.outH, splitX, p.srcW, p.srcH,
                  p.layout == 1 ? "tab" : "sbs", p.halfMode, p.swap,
                  eyeW, eyeH, imgW * sc, imgH * sc);
    }
    return p;
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
        node->owner = this;
        node->dev = dev_;
    }
    // 每帧同步 pipeline/layout：运行期 SDR↔HDR10 切换会重建 composite
    // pipeline（新 attachment 格式），节点若继续用旧句柄就是**已销毁对象**
    // （校验层 VUID-vkCmdBindPipeline-pipeline-parameter + 合成画面不更新）。
    node->pipe = pipe_;
    node->pipeLayout = pipeLayout_;
    if (auto *w = window()) {
        qreal dpr = w->devicePixelRatio();
        node->w = (int)(w->width() * dpr);
        node->h = (int)(w->height() * dpr);
    }

    // ── render target sizing (window framebuffer size) ────────────────
    // resize 节流：尺寸连续 RT_STABLE_FRAMES 帧未变才重建。拖动中
    // 每帧尺寸变化 → 只更新 pending（零重建）；停止后 ~10 帧重建一次
    //（2026-08-06 由 30 缩短——快速 resize 时重建曾被节流吞掉）。
    // 重建会触发 mpv reconfig（VSR 分辨率重配 + vsr_reconfigure），
    // 若每帧重建 → resize 拖动全程卡顿。
    if (node->w > 0 && node->h > 0) {
        // 目标尺寸 = 窗口 framebuffer 尺寸，宽度向上取偶（见 compositeWidth）。
        const int tgtW = compositeWidth(node->w), tgtH = node->h;
        if (formatDirty_) {
            // 格式切换（SDR↔HDR10）：不能用稳定计数节流——mpv 在同一帧稍后
            // 就会用新格式渲染，旧格式的 image 必须已经换掉。
            pendingW_ = tgtW;
            pendingH_ = tgtH;
            stableCount_ = RT_STABLE_FRAMES;
            formatDirty_ = false;
        }
        if (tgtW == pendingW_ && tgtH == pendingH_) {
            stableCount_++;
            if (stableCount_ >= RT_STABLE_FRAMES) {
                const int oldW = w_, oldH = h_;
                if (ensureRenderTarget(pendingW_, pendingH_) &&
                    (oldW != w_ || oldH != h_))
                    // 新建的 rt 是空图；没有新帧时 mpv 不会自己再画一次 →
                    // 必须本帧强制渲染当前帧，否则 resize 后画面全黑（暂停时
                    // 切 3D 实测：rt 追平了但内容没重画）。
                    renderRequested_.store(true);
            }
        } else {
            if (tgtW != pendingW_ || tgtH != pendingH_)
                VLOG("rt resize pending %dx%d (was %dx%d)",
                     tgtW, tgtH, pendingW_, pendingH_);
            pendingW_ = tgtW;
            pendingH_ = tgtH;
            stableCount_ = 0;
        }
        // 没有新帧时（暂停、或拖动结束后静止）updatePaintNode 不会再被调用，
        // 上面的"连续 N 次同尺寸"计数永远到不了阈值 → 渲染目标停在上一个尺寸。
        // 3D 下这会表现为画面只占半屏：mpv 仍把 3840×1080 塞进旧的 16:9 rt
        //（rt 未重建、composite 却按新窗口取样）。尺寸未追平时自我驱动补帧，
        // 计数到阈值（重建成功或失败）后条件自然为假，不会无界循环。
        if ((tgtW != w_ || tgtH != h_) && stableCount_ < RT_STABLE_FRAMES)
            update();
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
            mpv_->render(rtImage_, renderFormat_, w_, h_);
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
    // dirty 由 requestRender（mpv update callback invokeMethod 驱动）
    // 条件设置——这里绝不无条件 update()/postEvent（那会制造渲染
    // 事件风暴抢占事件循环，实测 EVT 400-1350ms 饿死输入；15a6f15
    // 的 aboutToBlock 无条件投递曾致 UI 卡顿回归，已删除）。

    return node;
}
