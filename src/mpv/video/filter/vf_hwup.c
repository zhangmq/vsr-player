// vf_hwup.c — SW→HW 一致化前置节点
//
// Pipeline: decode → [vf_hwup] → [vf_rife] → [vf_vsr] → VO
//
// SW 帧（任意 YUV）上传为 CUDA 帧（NV12/P010，位深匹配），使下游
// rife/vsr 只处理 HW 路径（rife/vsr 不再需要各自的 SW 支持）。
// HW CUDA 帧（hwdec NV12/P010）原样直通。
//
// 输出帧 hwctx = 自建 av_hw_frames，尺寸 = max(RT, 视频)。
// RT 尺寸不是给 rife 的（rife 输出帧 = 视频尺寸，借用视频尺寸 hwctx
// 足够）——是给 vsr 输出帧 + 软解场景的 VO 下载路径的：软解输入无
// hwdec → VO 无 CUDA interop（hwdec_devs 空）→ autoconvert 插
// hwdownload → av_hwframe_transfer_data 按帧尺寸从 hwctx 解析，hwctx
// < 帧尺寸（vsr 输出 = 视频×scale）越界崩溃（实测 mp_image_hw_download
// assert）。RT 尺寸 ≥ 视频×scale 的常见输出，因此 hwctx = max(RT, 视频)
// 使下载路径安全；窗口小于视频时用视频尺寸（帧尺寸匹配）。
//
// CUDA context: 自建（SW 输入无 hwdec 帧可借）。单 ctx 设计延续——下游
// rife/vsr 通过帧 hwctx 借用本 filter 的 ctx，全链共享（多 ctx 并发挂
// GPU 教训，见 cuda_shared.h）。

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "common/msg.h"

#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include <cuda.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cuda_shared.h"

// ── Filter private state ───────────────────────────────────────────────────
// CUDA context ownership: shared struct (cuda_shared.h). This filter is the
// CREATOR for SW input (owned=true, self-created context); HW frames pass
// through without ever touching CUDA. Output frames carry the shared struct
// as bufs[0] opaque — the context lives as long as any frame references it,
// so frames that outlive the filter (pin teardown after f_destroy) can still
// release their buffer on a live context.

struct priv {
    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    AVBufferRef *av_hw_device;   // CUDA device ctx (wraps cuda_ref->ctx)
    AVBufferRef *av_hw_frames;   // CUDA/NV12·P010 frames ctx (max(RT, 视频))
    CUcontext    av_hw_frames_ctx;  // ctx av_hw_frames was created on

    CUdeviceptr out_buf;         // Y + UV 两区段（单 buf）
    size_t      out_buf_size;
};

// ── AVBufferRef free callback for CUDA memory ──────────────────────────────

static void mp_image_dtor(void *ptr)
{
    struct mp_image *mpi = ptr;
    for (int p = 0; p < MP_MAX_PLANES; p++)
        av_buffer_unref(&mpi->bufs[p]);
    av_buffer_unref(&mpi->hwctx);
    av_buffer_unref(&mpi->icc_profile);
    av_buffer_unref(&mpi->a53_cc);
    av_buffer_unref(&mpi->dovi);
    av_buffer_unref(&mpi->film_grain);
    for (int n = 0; n < mpi->num_ff_side_data; n++)
        av_buffer_unref(&mpi->ff_side_data[n].buf);
}

static void free_cuda_buf(void *opaque, uint8_t *data)
{
    struct mp_cuda_ctx_ref *r = opaque;
    if (r && r->ctx) cuCtxPushCurrent(r->ctx);
    // No stream sync — see vf_vsr.c free_cuda_buf for the reasoning: the
    // buffer is only freed after its consumers (rife/vsr's copies on their
    // streams, or the VO's map copy under retained-frame semantics) are done,
    // and a stream-0 sync on the shared context would wait on the render
    // thread.
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        mp_cuda_ctx_ref_release(r);
}

// ── CUDA context setup ─────────────────────────────────────────────────────
// SW frames have no hwctx to borrow from — the filter creates its own context
// (mirror vf_vsr.c's SW branch). Once created it lives for the filter's whole
// lifetime: decode output is either all-HW or all-SW (no per-frame switching),
// and HW frames pass through without touching CUDA at all.

static bool ensure_cuda(struct mp_filter *f, struct priv *p)
{
    if (p->cuda_ref)
        return true;

    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "hwup: cuInit failed (%d)\n", cr);
        return false;
    }
    CUdevice dev;
    cr = cuDeviceGet(&dev, 0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "hwup: cuDeviceGet failed (%d)\n", cr);
        return false;
    }
    CUcontext ctx;
    cr = cuCtxCreate(&ctx, CU_CTX_SCHED_AUTO, dev, 0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "hwup: cuCtxCreate failed (%d)\n", cr);
        return false;
    }

    p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
    p->cuda_ref->magic = MP_CUDA_CTX_REF_MAGIC;
    p->cuda_ref->ctx = ctx;
    p->cuda_ref->owned = true;
    atomic_init(&p->cuda_ref->refs, 1);

    cuCtxPushCurrent(ctx);
    cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_NON_BLOCKING);
    cuCtxPopCurrent(NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "hwup: cuStreamCreate failed (%d)\n", cr);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        return false;
    }
    p->cuda_init_done = true;
    MP_VERBOSE(f, "hwup: created CUDA context on device 0 (SW input)\n");
    return true;
}

// ── AVHWFramesContext for the output CUDA frames ───────────────────────────
// Upload frames carry a self-created NV12/P010 frames ctx (hwctx sw_format
// must match the frame's subfmt — shared helper mp_cuda_ensure_frames).

static bool ensure_av_hw_frames(struct mp_filter *f, struct priv *p,
                                enum mp_imgfmt swfmt, int w, int h)
{
    if (!p->cuda_init_done) return false;
    bool had = p->av_hw_frames != NULL;
    if (!mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx, imgfmt2pixfmt(swfmt),
                               w, h, &p->av_hw_device, &p->av_hw_frames,
                               &p->av_hw_frames_ctx))
        return false;
    if (!had)
        MP_VERBOSE(f, "hwup: CUDA/%s hw_frames_ctx %dx%d\n",
                   mp_imgfmt_to_name(swfmt), w, h);
    return true;
}

// ── f_process ──────────────────────────────────────────────────────────────

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);

    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO) {
        MP_ERR(f, "hwup: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *mpi = frame.data;

    // ── HW CUDA 帧：直通（hwdec NV12/P010，下游 rife/vsr 直接可用）──
    if (mpi->imgfmt == IMGFMT_CUDA) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // ── SW 帧：上传为 CUDA 帧（NV12/P010，位深匹配）────────────────
    if (!ensure_cuda(f, p)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(mpi->imgfmt);
    int bd = desc.bpp[0];
    // mpv 无 P016：16bit+ 输入截断到 P010（保留高 10 位，sws 处理）
    enum mp_imgfmt fmt = bd <= 8 ? IMGFMT_NV12 : IMGFMT_P010;
    int bpp = bd <= 8 ? 1 : 2;

    // RT 尺寸（照抄 vf_vsr resolve_scale 的获取方式：libmpv 非阻塞读
    // 缓存，VO 未 render 过为 0）。hwctx 尺寸 = max(RT, 视频)——见文件
    // 头注释：为 vsr 输出帧 + 软解场景 VO 的 hwdownload 路径服务
    int hw_w = mpi->w, hw_h = mpi->h;
    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    if (info && info->get_render_target_size) {
        int res[2] = {0};
        info->get_render_target_size(info, res);
        if (res[0] > hw_w) hw_w = res[0];
        if (res[1] > hw_h) hw_h = res[1];
    }

    if (!ensure_av_hw_frames(f, p, fmt, hw_w, hw_h)) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    // sws: 任意 YUV → NV12/P010（CPU）。range 不转换（sws 默认保持源
    // levels）——输出帧显式带出源 repr，下游 yuv_to_rgba 按 levels 正确
    // 转换（levels 标记丢失曾致 VO 交替帧闪烁）
    struct mp_image *nv12 = mp_image_alloc(fmt, mpi->w, mpi->h);
    if (!nv12) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    mp_image_copy_attributes(nv12, mpi);
    struct mp_sws_context *sws = mp_sws_alloc(f);
    if (!sws) {
        talloc_free(nv12);
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    sws->force_scaler = MP_SWS_AUTO;
    if (mp_sws_scale(sws, nv12, mpi) < 0) {
        MP_ERR(f, "hwup: sws_scale failed\n");
        talloc_free(sws);
        talloc_free(nv12);
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    talloc_free(sws);

    // 输出缓冲：Y + UV 两区段（单 buf，与 hwdec NV12 帧同布局）
    size_t y_bytes  = (size_t)mpi->w * mpi->h * bpp;
    size_t uv_bytes = (size_t)mpi->w * mpi->h / 2 * bpp;
    int pitch_y  = mpi->w * bpp;
    int pitch_uv = mpi->w * bpp;

    cuCtxPushCurrent(p->cuda_ref->ctx);
    CUresult cr = cuMemAlloc(&p->out_buf, y_bytes + uv_bytes);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "hwup: cuMemAlloc failed (%d)\n", cr);
        cuCtxPopCurrent(NULL);
        talloc_free(nv12);
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    p->out_buf_size = y_bytes + uv_bytes;

    {
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost       = nv12->planes[0];
        copy.srcPitch      = (size_t)nv12->stride[0];
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice     = p->out_buf;
        copy.dstPitch      = (size_t)pitch_y;
        copy.WidthInBytes  = (size_t)mpi->w * bpp;
        copy.Height        = (size_t)mpi->h;
        cr = cuMemcpy2DAsync(&copy, p->cuda_stream);
        if (cr != CUDA_SUCCESS) {
            MP_ERR(f, "hwup: Y upload failed (%d)\n", cr);
            cuMemFree(p->out_buf); p->out_buf = 0; p->out_buf_size = 0;
            cuCtxPopCurrent(NULL);
            talloc_free(nv12);
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            return;
        }
        copy.srcHost  = nv12->planes[1];
        copy.srcPitch = (size_t)nv12->stride[1];
        copy.dstDevice = p->out_buf + y_bytes;
        copy.dstPitch = (size_t)pitch_uv;
        copy.WidthInBytes = (size_t)mpi->w * bpp;
        copy.Height       = (size_t)mpi->h / 2;
        cr = cuMemcpy2DAsync(&copy, p->cuda_stream);
        if (cr != CUDA_SUCCESS) {
            MP_ERR(f, "hwup: UV upload failed (%d)\n", cr);
            cuMemFree(p->out_buf); p->out_buf = 0; p->out_buf_size = 0;
            cuCtxPopCurrent(NULL);
            talloc_free(nv12);
            mp_frame_unref(&frame);
            mp_filter_internal_mark_failed(f);
            return;
        }
        // 上传完成才交出帧：下游（rife/vsr）在自己的 stream 上读本帧，
        // 不同 stream 间无隐式同步（vsr SW 路径同款）
        cuStreamSynchronize(p->cuda_stream);
    }
    talloc_free(nv12);

    // ── 输出帧 ────────────────────────────────────────────────────────
    // 每帧一个 cuMemAlloc，随帧释放（free_cuda_buf）——与 vf_vsr 相同
    // 模式。帧持共享 cuda_ref 引用（bufs[0] opaque），可活过 filter——
    // ctx 活到最后一帧（下游借者也引用同一结构）。
    mp_cuda_ctx_ref_acquire(p->cuda_ref);
    struct mp_image *out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t *)p->out_buf, (int)p->out_buf_size,
                                    free_cuda_buf, p->cuda_ref, 0);
    if (!out->bufs[0]) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        talloc_free(out);
        cuMemFree(p->out_buf); p->out_buf = 0; p->out_buf_size = 0;
        cuCtxPopCurrent(NULL);
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }
    mp_image_setfmt(out, IMGFMT_CUDA);
    mp_image_sethwfmt(out, IMGFMT_CUDA, fmt);
    mp_image_params_guess_csp(&out->params);
    out->params.color = mpi->params.color;   // HDR metadata 保留
    out->params.repr  = mpi->params.repr;    // 源 levels 保留——NV12 未转
                                             // range，下游按 levels 转换
    out->params.p_w = mpi->params.p_w;
    out->params.p_h = mpi->params.p_h;
    out->w = mpi->w; out->h = mpi->h;
    out->params.w = mpi->w; out->params.h = mpi->h;
    out->planes[0] = (uint8_t *)p->out_buf;
    out->planes[1] = (uint8_t *)p->out_buf + y_bytes;
    out->stride[0] = pitch_y;
    out->stride[1] = pitch_uv;
    out->num_planes = 2;
    out->hwctx = av_buffer_ref(p->av_hw_frames);
    out->pts = mpi->pts;
    out->dts = mpi->dts;
    out->nominal_fps = mpi->nominal_fps;
    p->out_buf = 0; p->out_buf_size = 0;
    cuCtxPopCurrent(NULL);

    MP_DBG(f, "hwup: %s %dx%d -> CUDA/%s\n",
           mp_imgfmt_to_name(mpi->imgfmt), mpi->w, mpi->h,
           mp_imgfmt_to_name(fmt));

    talloc_free(mpi);
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
}

// ── f_destroy ──────────────────────────────────────────────────────────────

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    bool ctx_pushed = false;
    if (p->cuda_ref && p->cuda_ref->ctx) {
        CUresult cr = cuCtxPushCurrent(p->cuda_ref->ctx);
        ctx_pushed = (cr == CUDA_SUCCESS);
    }

    if (p->out_buf) {
        if (ctx_pushed) cuMemFree(p->out_buf);
        p->out_buf      = 0;
        p->out_buf_size = 0;
    }
    if (p->cuda_init_done) {
        if (ctx_pushed) cuStreamDestroy(p->cuda_stream);
        p->cuda_stream = NULL;
    }
    if (ctx_pushed)
        cuCtxPopCurrent(NULL);

    // frames/device 载体释放必须发生在无 push 状态：其 uninit 可能
    // cuCtxDestroy（FFmpeg 自建的 ctx / hwdec 载体）。current_ctx 模式下
    // 借的 ctx 不在此销毁——由共享结构 refs 管理。
    if (p->av_hw_frames) {
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames = NULL;
    }
    if (p->av_hw_device) {
        av_buffer_unref(&p->av_hw_device);
        p->av_hw_device = NULL;
    }

    // Release the filter's context reference. Frames still in flight
    // (pin teardown happens after destroy()) hold their own references;
    // the context is destroyed only when the last one is released.
    if (p->cuda_ref) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
    }
    p->cuda_init_done = false;
}

// ── Registration ───────────────────────────────────────────────────────────

static const struct mp_filter_info vf_hwup_filter = {
    .name      = "hwup",
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .destroy   = f_destroy,
};

static struct mp_filter *vf_hwup_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_hwup_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    return f;
}

const struct mp_user_filter_entry vf_hwup = {
    .desc = {
        .name        = "hwup",
        .description = "SW→HW 一致化（SW YUV 上传为 CUDA 帧，前置 rife/vsr）",
    },
    .create = vf_hwup_create,
};
