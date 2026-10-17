/*
 * libmpv_vk.c — Vulkan render backend for mpv render API
 *
 * Host provides VkInstance + VkDevice at init, VkImage per frame.
 * mpv imports the device via pl_vulkan_import(), wraps user VkImage
 * via pl_vulkan_wrap(), renders via gl_video_render_frame(),
 * and signals completion via semaphore.
 */

#include <stdio.h>
#include <string.h>

#include <libplacebo/vulkan.h>

#include "common.h"
#include "context.h"
#include "options/m_config.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/gpu/ra.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "video/out/vulkan/common.h"

#include "mpv/render.h"
#include "mpv/render_vk.h"

struct priv {
    // ═══ mpvk_ctx pointer — FIRST field, required by ra_vk_ctx_get() ═══
    // context.c:ra_vk_ctx_get() casts swapchain->priv to (mpvk_ctx**) and
    // dereferences it. This enables vulkan hwdec interop (hwdec_vulkan.c)
    // for the libmpv vulkan backend.
    struct mpvk_ctx *vk;

    struct ra_ctx *ra_ctx;
    pl_vulkan vulkan;
    pl_log       pllog;

    // Instance-level info (not in pl_vulkan), stored for fake pl_vk_inst
    VkInstance                 host_instance;
    PFN_vkGetInstanceProcAddr  host_get_proc_addr;
    const char * const        *host_inst_extensions;
    int                        host_num_inst_extensions;

    // 已打过日志的格式（fmt/hdr 变化时打一条 INFO；日志纪律：状态变更）
    VkFormat    logged_format;
    int         logged_hdr;

    // Per-frame
    VkImage     cur_image;
    VkSemaphore cur_signal_sem;
    uint64_t    cur_signal_value;
    int         cur_w, cur_h;
    // 宿主声明的 render target 格式与 HDR 目标（每帧由 wrap_fbo 从
    // mpv_vulkan_image 抄入；vk_start_frame / vk_color_depth 读它）
    VkFormat    cur_format;
    int         cur_hdr;
    float       cur_min_luma, cur_max_luma;

    // Cached wrap — makes wrap_fbo idempotent across double-call
    VkImage     cached_image;
    pl_tex      cached_pltex;
    struct ra_tex *cached_ratex;

    // Timeline semaphore for hold_ex (incrementing value, reusable)
    VkSemaphore  hold_sem;
    uint64_t     hold_val;
};

// ── Swapchain ────────────────────────────────────────────────────────

// 目标位深（mpv 只把它喂给 gl_video_set_fb_depth，决定 dither 深度，
// 不改格式/色彩空间）。⚠ 顺序依赖：vo_gpu 仅在 resize() 里读它
// （video/out/vo_gpu.c:65），而 resize() 可能发生在首个 wrap_fbo 之前 →
// 首帧之前拿到的是 cur_format 的初始值 0（返回 8，dither 略浅）；窗口尺寸
// 变化后会重新读到正确值。cur_format 由 wrap_fbo（宿主每帧调用，早于
// start_frame）写入，是链路里最早能拿到宿主格式的位置。
static int vk_color_depth(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;
    switch (p->cur_format) {
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 10;
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return 16;  // scRGB：FP16 无需 dither
    default:                                 return 8;
    }
}

// ra_swapchain_fns.target_csp：vo_gpu 当前**不消费**它（只有 vo_gpu_next 用），
// 真正生效的宿主色彩空间走 vk_start_frame 填的 ra_fbo.color_space
// （见这里的 vk_start_frame 与 gpu/context.h:105 的语义说明）。
// 保持 {0}（unspecified）与 GL 后端一致。
static struct pl_color_space vk_target_csp(struct ra_swapchain *sw)
{
    return (struct pl_color_space){0};
}

static bool vk_start_frame(struct ra_swapchain *sw, struct ra_fbo *out_fbo)
{
    struct priv *p = sw->priv;

    if (!p->cur_image)
        return false;

    // 状态变更日志（MP_INFO ≤ 客户端的 all=status，可见）：一条即可证明
    // ① pl_vulkan_wrap 用的就是宿主给的格式；② vk_color_depth() 的计算口径。
    if (p->cur_format != p->logged_format || p->cur_hdr != p->logged_hdr) {
        p->logged_format = p->cur_format;
        p->logged_hdr    = p->cur_hdr;
        MP_INFO(sw->ctx, "libmpv_vk: render target fmt=%d hdr=%d "
                "color_depth=%d csp=%s（合成器亮度 min=%.2f max=%.2f nit，仅诊断；"
                "故意不填进 csp —— 否则会本地预 tone map 到 203nit 变暗）\n",
                (int)p->cur_format, p->cur_hdr, vk_color_depth(sw),
                p->cur_hdr ? "PQ/BT.2020" : "unspecified",
                p->cur_min_luma, p->cur_max_luma);
    }

    pl_gpu gpu = p->vulkan->gpu;

    // 宿主声明的目标色彩空间：hdr → HDR10（PQ/BT.2020 + 显示亮度），
    // 否则 {0}（unspecified，与改动前一致）。缓存快路径也必须带它一起返回
    // —— mpv 每帧会调用 start_frame 两次，第二次命中缓存。
    // ra_fbo.color_space 保持 {0}（与 mpv 自己的 vulkan backend 一致）：
    // ⚠ generic libmpv 渲染路径（video/out/gpu/libmpv_gpu.c 的 render()）
    // 自己拼 `struct ra_fbo {.tex, .flip}`，**会丢掉这里的 color_space**，
    // 所以"HDR 目标色彩空间"必须由宿主通过 mpv 选项声明
    //（target-trc/target-prim，见 MpvController::setHdrTarget）——实测：
    // 只填 primaries 不填 raw hdr.prim 还会撞上 libplacebo 的错误矩阵路径
    //（整幅品红/青）。亮度值（cur_min/max_luma）仍逐帧带下来，仅用于诊断
    // 日志（预 tone map 到 203nit 会让画面暗 2.4 倍，已实测排除该做法）。
    const struct pl_color_space csp = (struct pl_color_space) {0};
    (void)csp;

    // Idempotent: reuse cached wrap if same image (called twice per frame)
    if (p->cur_image == p->cached_image && p->cached_ratex) {
        *out_fbo = (struct ra_fbo) { .tex = p->cached_ratex, .flip = false,
                                     .color_space = csp };
        return true;
    }

    // Discard previous cache
    if (p->cached_pltex) {
        pl_tex_destroy(gpu, &p->cached_pltex);
        p->cached_pltex = NULL;
    }
    if (p->cached_ratex) {
        talloc_free(p->cached_ratex);
        p->cached_ratex = NULL;
    }

    struct pl_vulkan_wrap_params wrap = {
        .image  = p->cur_image,
        .width  = p->cur_w,
        .height = p->cur_h,
        .format = p->cur_format,   // 宿主给的格式（SDR=R8G8B8A8，HDR10=A2B10G10R10）

        .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT,
    };

    pl_tex pltex = pl_vulkan_wrap(gpu, &wrap);
    if (!pltex)
        return false;

    pl_vulkan_release_ex(gpu, &(struct pl_vulkan_release_params) {
        .tex    = pltex,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .qf     = VK_QUEUE_FAMILY_IGNORED,
    });

    struct ra_tex *ratex = talloc_zero(NULL, struct ra_tex);
    if (!mppl_wrap_tex(sw->ctx->ra, pltex, ratex)) {
        pl_tex_destroy(gpu, &pltex);
        talloc_free(ratex);
        return false;
    }

    p->cached_image = p->cur_image;
    p->cached_pltex = pltex;
    p->cached_ratex = ratex;

    *out_fbo = (struct ra_fbo) { .tex = ratex, .flip = false,
                                 .color_space = csp };
    return true;
}

static bool vk_submit_frame(struct ra_swapchain *sw, const struct vo_frame *frame)
{
    struct priv *p = sw->priv;

    pl_gpu_flush(p->vulkan->gpu);

    // Hold image back, transitioning to GENERAL for host sampling.
    // Timeline semaphore value increments each frame, so it can be
    // reused without GPU hang.
    if (p->cached_pltex) {
        p->hold_val++;
        pl_vulkan_hold_ex(p->vulkan->gpu, pl_vulkan_hold_params(
            .tex       = p->cached_pltex,
            .layout    = VK_IMAGE_LAYOUT_GENERAL,
            .semaphore = { .sem = p->hold_sem, .value = p->hold_val },
        ));
    }

    // Signal the host-provided timeline semaphore
    if (p->cur_signal_sem) {
        VkSemaphoreSignalInfo ssi = {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = p->cur_signal_sem,
            .value     = p->cur_signal_value,
        };
        vkSignalSemaphore(p->vulkan->device, &ssi);
    }

    if (p->cached_pltex) {
        pl_tex_destroy(p->vulkan->gpu, &p->cached_pltex);
        p->cached_pltex = NULL;
    }
    if (p->cached_ratex) {
        talloc_free(p->cached_ratex);
        p->cached_ratex = NULL;
    }
    p->cached_image = VK_NULL_HANDLE;
    p->cur_image       = VK_NULL_HANDLE;
    p->cur_signal_sem  = VK_NULL_HANDLE;
    p->cur_signal_value = 0;
    return true;
}

static void vk_swap_buffers(struct ra_swapchain *sw) {}
static void vk_get_vsync(struct ra_swapchain *sw, struct vo_vsync_info *i) {}
static bool vk_set_color(struct ra_swapchain *sw, struct mp_image_params *p) { return true; }

// Exported (non-static): context.c:ra_vk_ctx_get() references this symbol
// to detect libmpv vulkan swapchains and return the embedded mpvk_ctx.
const struct ra_swapchain_fns libmpv_vk_swapchain_fns = {
    .color_depth   = vk_color_depth,
    .set_color     = vk_set_color,
    .target_csp    = vk_target_csp,
    .start_frame   = vk_start_frame,
    .submit_frame  = vk_submit_frame,
    .swap_buffers  = vk_swap_buffers,
    .get_vsync     = vk_get_vsync,
};

// ── libmpv_gpu_context_fns ───────────────────────────────────────────

static int init(struct libmpv_gpu_context *ctx, mpv_render_param *params)
{
    mpv_vulkan_init_params *vk_init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!vk_init || !vk_init->instance || !vk_init->device || !vk_init->physical_device)
        return MPV_ERROR_INVALID_PARAMETER;

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    p->pllog = mppl_log_create(p, ctx->log);

    // Stash instance-level info for fake pl_vk_inst (needed by hwdec_vulkan.c)
    p->host_instance          = vk_init->instance;
    p->host_get_proc_addr     = vk_init->get_proc_addr;
    p->host_inst_extensions   = vk_init->extensions;
    p->host_num_inst_extensions = vk_init->num_extensions;

    struct pl_vulkan_import_params import = {
        .instance       = vk_init->instance,
        .get_proc_addr  = vk_init->get_proc_addr,
        .phys_device    = vk_init->physical_device,
        .device         = vk_init->device,
        .queue_graphics = {
            .index = vk_init->graphics_queue_family,
            .count = vk_init->graphics_queue_count,
        },
        .queue_compute = {
            .index = vk_init->compute_queue_family,
            .count = vk_init->compute_queue_count,
        },
        .queue_transfer = {
            .index = vk_init->transfer_queue_family,
            .count = vk_init->transfer_queue_count,
        },
        .extensions     = vk_init->extensions,
        .num_extensions = vk_init->num_extensions,
        .features       = vk_init->features,
    };

    p->vulkan = pl_vulkan_import(p->pllog, &import);
    if (!p->vulkan)
        return MPV_ERROR_UNSUPPORTED;

    struct ra *ra = ra_create_pl(p->vulkan->gpu, ctx->log);
    if (!ra) {
        pl_vulkan_destroy(&p->vulkan);
        return MPV_ERROR_UNSUPPORTED;
    }

    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log    = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->ra     = ra;
    p->ra_ctx->vo     = talloc_zero(p->ra_ctx, struct vo);
    p->ra_ctx->vo->global = ctx->global;

    p->ra_ctx->swapchain = talloc_zero(p, struct ra_swapchain);
    p->ra_ctx->swapchain->ctx  = p->ra_ctx;
    p->ra_ctx->swapchain->fns  = &libmpv_vk_swapchain_fns;
    p->ra_ctx->swapchain->priv = p;  // first field of priv is mpvk_ctx*

    // Timeline semaphore for hold_ex (reusable, unlike binary)
    VkSemaphoreTypeCreateInfo stci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stci.initialValue = 0;
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci};
    vkCreateSemaphore(p->vulkan->device, &sci, NULL, &p->hold_sem);
    p->hold_val = 0;

    // ── Build mpvk_ctx for hwdec interop ─────────────────────────────
    // hwdec_vulkan.c calls ra_vk_ctx_get() which returns priv->vk.
    // We populate a minimal mpvk_ctx with the handles the interop needs.
    // The fake pl_vk_inst is NOT created by pl_vk_inst_create() — it only
    // provides instance/extensions/get_proc_addr for FFmpeg's
    // AVHWDeviceContext. ra_vk_ctx_uninit() is never called on our
    // ra_ctx (we handle cleanup in destroy()), so it won't try to
    // pl_vk_inst_destroy() this fake.
    struct pl_vk_inst_t *vkinst = talloc_zero(p, struct pl_vk_inst_t);
    vkinst->instance       = p->host_instance;
    vkinst->get_proc_addr  = p->host_get_proc_addr;
    vkinst->extensions     = p->host_inst_extensions;
    vkinst->num_extensions = p->host_num_inst_extensions;

    p->vk = talloc_zero(p, struct mpvk_ctx);
    p->vk->pllog     = p->pllog;
    p->vk->vkinst    = vkinst;
    p->vk->vulkan    = p->vulkan;
    p->vk->gpu       = p->vulkan->gpu;
    p->vk->swapchain = NULL;       // we have no pl_swapchain
    p->vk->surface   = VK_NULL_HANDLE;

    ctx->ra_ctx = p->ra_ctx;
    return 0;
}

static int wrap_fbo(struct libmpv_gpu_context *ctx, mpv_render_param *params,
                    struct ra_tex **out)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_image *img =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_IMAGE, NULL);
    if (!img || !img->image)
        return MPV_ERROR_INVALID_PARAMETER;

    p->cur_image        = img->image;
    p->cur_signal_sem   = img->signal_semaphore;
    p->cur_signal_value = img->signal_semaphore_value;
    p->cur_w = img->width;
    p->cur_h = img->height;
    // 目标格式与色彩空间（本项目扩展字段；旧调用方不填 → 0 = SDR 行为不变）
    p->cur_format   = img->format;
    p->cur_hdr      = img->hdr;
    p->cur_min_luma = img->min_luma;
    p->cur_max_luma = img->max_luma;

    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    struct ra_fbo target;
    if (!sw->fns->start_frame(sw, &target))
        return MPV_ERROR_UNSUPPORTED;
    // 把 FBO 色彩空间（vk_start_frame() 按宿主 HDR 状态填的）交给
    // libmpv_gpu.c 的 render()：上游会自己拼 fbo 丢掉它 → HDR 下 gl_video
    // 误判 FBO 未指定 → PQ 内容被烘成 gamma2.2 写进 PQ swapchain（实测暗 2.4 倍）。

    *out = target.tex;
    return 0;
}

static void done_frame(struct libmpv_gpu_context *ctx, bool ds)
{
    struct priv *p = ctx->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    struct vo_frame dummy = { .display_synced = ds };
    sw->fns->submit_frame(sw, &dummy);
}

static void destroy(struct libmpv_gpu_context *ctx)
{
    struct priv *p = ctx->priv;
    if (p->hold_sem)
        vkDestroySemaphore(p->vulkan->device, p->hold_sem, NULL);
    if (p->cached_pltex) {
        pl_tex_destroy(p->vulkan->gpu, &p->cached_pltex);
        p->cached_pltex = NULL;
    }
    if (p->cached_ratex) {
        talloc_free(p->cached_ratex);
        p->cached_ratex = NULL;
    }
    if (p->ra_ctx) {
        if (p->ra_ctx->ra)
            talloc_free(p->ra_ctx->ra);
        talloc_free(p->ra_ctx);
    }
    if (p->vulkan)
        pl_vulkan_destroy(&p->vulkan);
    if (p->pllog)
        pl_log_destroy(&p->pllog);
}

const struct libmpv_gpu_context_fns libmpv_gpu_context_vk = {
    .api_name   = MPV_RENDER_API_TYPE_VULKAN,
    .init       = init,
    .wrap_fbo   = wrap_fbo,
    .done_frame = done_frame,
    .destroy    = destroy,
};
