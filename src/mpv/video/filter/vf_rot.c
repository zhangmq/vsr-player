// vf_rot.c — 90°/180°/270° 旋转（CUDA 帧，零拷贝 D2D）
//
// 链上位置：vf_hwup 之后、vf_vsr/vf_vfg 之前（输入恒为 IMGFMT_CUDA，
// hw_subfmt = NV12 或 P010）。用 NVRTC 编译的 kernel 直接读/写显存平面——
// 没有 hwdownload/hwupload、没有 CPU 拷贝。
//
// angle: 0=off、90=顺时针、180、270=顺时针 270°（= 逆时针 90°）。
// 90/270 交换宽高；180 不变。angle=0 时直通（链上恒有这个占位滤镜，
// 运行期 `vf-command rot angle N` 切换，不重建链）。
//
// 输出帧：新 cuMemAlloc（Y + UV 两区段，与 hwup/vsr 同布局），hwctx 是
// **旋转后尺寸**的 CUDA/NV12|P010 frames ctx（mp_cuda_ensure_frames）——
// 末端的 hwdownload 按 hwctx 尺寸解析，借用源尺寸的 hwctx 会错乱/崩溃。
//
// 旋转后像素宽高比（p_w/p_h）交换：显示宽高 = w*p_w / h*p_h，旋转 90°
// 后要保持内容形状，p_w/p_h 必须互换（否则 VO 按方形像素渲染成错误比例）。
// 文件自带的 rotate 元数据（手机视频）原样带出：菜单角度是**叠加**在它
// 之上的（与 --video-rotate 的"加到元数据"语义一致）。

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "common/msg.h"
#include "options/m_option.h"

#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include <cuda.h>
#include <nvrtc.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuda_shared.h"

// ── Options ────────────────────────────────────────────────────────────────

struct vf_rot_opts {
    int angle;   // 0=off, 90/180/270（顺时针）
};

#define OPT_BASE_STRUCT struct vf_rot_opts

static const struct m_option vf_opts_fields[] = {
    {"angle", OPT_CHOICE(angle,
        {"0", 0}, {"90", 90}, {"180", 180}, {"270", 270}),
        M_RANGE(0, 270)},
    {0}
};

static const struct vf_rot_opts vf_rot_opts_def = {
    .angle = 0,
};

// 进程内"最后一次 vf-command 的 angle"。滤镜实例被 mpv 重建时用它覆盖链串里的
// 初始值——链串是**加载时**的快照，若与当前状态不一致且该选项会改变输出尺寸
// （rot 90/270 交换 w/h），重建后尺寸再变一次 → 又触发重配 → 无限重建/尺寸来回
// 翻转（实测 2026-10-27：关掉旋转后每次重建又回到 90，日志刷出几十轮
// "rot: 90 deg ... -> angle 90 -> 0"）。
// -1 = 还没被命令设置过（用链串值）。
static _Atomic int g_last_angle = -1;

// ── Filter private state ───────────────────────────────────────────────────

struct priv {
    struct vf_rot_opts *opts;

    // CUDA context 借用（与 vf_vsr/vf_vfg 同构，见 cuda_shared.h）：
    // 上游共享结构（hwup 输出帧）→ acquire 同一结构；hwdec 帧（opaque 非
    // 本链结构）→ 自建借者结构 + ctx_holder。
    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream  cuda_stream;

    AVBufferRef *av_hw_device;
    AVBufferRef *av_hw_frames;      // CUDA/NV12|P010，**旋转后尺寸**
    CUcontext    av_hw_frames_ctx;

    CUmodule   module;              // 每 context 编译一次
    CUfunction k8, k16, k32;        // 8-bit / 16-bit / 32-bit 元素转置

    bool warned;                    // 不支持/失败只警告一次
    int  last_in_w, last_in_h, last_angle;  // 日志去重
};

// ── Kernel source ─────────────────────────────────────────────────────────
// 9 个参数：src, src_pitch(elements), dst, dst_pitch(elements),
//           sw, sh（源平面尺寸）, dw, dh（目标平面尺寸）, mode
// mode: 1=顺时针 90°，2=逆时针 90°，3=180°。
// 90/270 时 dw=sh、dh=sw；180 时 dw=sw、dh=sh。

static const char *kKernelSrc =
"extern \"C\" __global__ void rot_u8(const unsigned char *__restrict__ s, int sp,\n"
"    unsigned char *__restrict__ d, int dp, int sw, int sh, int dw, int dh, int mode)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    int sx, sy;\n"
"    if (mode == 1)      { sx = y;         sy = sh - 1 - x; }\n"
"    else if (mode == 2) { sx = sw - 1 - y; sy = x; }\n"
"    else                { sx = sw - 1 - x; sy = sh - 1 - y; }\n"
"    d[(long)y * dp + x] = s[(long)sy * sp + sx];\n"
"}\n"
"extern \"C\" __global__ void rot_u16(const unsigned short *__restrict__ s, int sp,\n"
"    unsigned short *__restrict__ d, int dp, int sw, int sh, int dw, int dh, int mode)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    int sx, sy;\n"
"    if (mode == 1)      { sx = y;         sy = sh - 1 - x; }\n"
"    else if (mode == 2) { sx = sw - 1 - y; sy = x; }\n"
"    else                { sx = sw - 1 - x; sy = sh - 1 - y; }\n"
"    d[(long)y * dp + x] = s[(long)sy * sp + sx];\n"
"}\n"
"extern \"C\" __global__ void rot_u32(const unsigned int *__restrict__ s, int sp,\n"
"    unsigned int *__restrict__ d, int dp, int sw, int sh, int dw, int dh, int mode)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    int sx, sy;\n"
"    if (mode == 1)      { sx = y;         sy = sh - 1 - x; }\n"
"    else if (mode == 2) { sx = sw - 1 - y; sy = x; }\n"
"    else                { sx = sw - 1 - x; sy = sh - 1 - y; }\n"
"    d[(long)y * dp + x] = s[(long)sy * sp + sx];\n"
"}\n";

// ── Frame helpers（与 vf_hwup.c 同款）─────────────────────────────────────

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
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        mp_cuda_ctx_ref_release(r);
}

// ── Kernel compile（NVRTC，一次/context）──────────────────────────────────

static bool ensure_kernels(struct mp_filter *f, struct priv *p)
{
    if (p->module)
        return true;

    nvrtcProgram prog;
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc, "rot_kernel",
                                         0, NULL, NULL);
    if (res != NVRTC_SUCCESS) {
        MP_ERR(f, "rot: nvrtcCreateProgram failed: %s\n",
               nvrtcGetErrorString(res));
        return false;
    }

    int major = 0, minor = 0;
    CUdevice dev;
    cuCtxGetDevice(&dev);
    cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    char arch[32];
    snprintf(arch, sizeof(arch), "--gpu-architecture=compute_%d%d", major, minor);
    const char *opts[] = {arch};

    res = nvrtcCompileProgram(prog, 1, opts);
    if (res != NVRTC_SUCCESS) {
        size_t log_size = 0;
        nvrtcGetProgramLogSize(prog, &log_size);
        char *log = malloc(log_size ? log_size : 1);
        if (log) {
            nvrtcGetProgramLog(prog, log);
            MP_ERR(f, "rot: NVRTC compile failed:\n%s\n", log);
            free(log);
        }
        nvrtcDestroyProgram(&prog);
        return false;
    }

    size_t ptx_size = 0;
    nvrtcGetPTXSize(prog, &ptx_size);
    char *ptx = malloc(ptx_size);
    if (!ptx) {
        nvrtcDestroyProgram(&prog);
        return false;
    }
    nvrtcGetPTX(prog, ptx);
    nvrtcDestroyProgram(&prog);

    CUresult cr = cuModuleLoadData(&p->module, ptx);
    free(ptx);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "rot: cuModuleLoadData failed (%d)\n", cr);
        p->module = NULL;
        return false;
    }
    if (cuModuleGetFunction(&p->k8, p->module, "rot_u8") != CUDA_SUCCESS ||
        cuModuleGetFunction(&p->k16, p->module, "rot_u16") != CUDA_SUCCESS ||
        cuModuleGetFunction(&p->k32, p->module, "rot_u32") != CUDA_SUCCESS) {
        MP_ERR(f, "rot: cuModuleGetFunction failed\n");
        cuModuleUnload(p->module);
        p->module = NULL;
        p->k8 = p->k16 = p->k32 = NULL;
        return false;
    }
    MP_INFO(f, "rot: kernels compiled (sm_%d%d)\n", major, minor);
    return true;
}

// ── CUDA context / frames（借上游，不新建 context）────────────────────────

static void reset_cuda(struct mp_filter *f, struct priv *p)
{
    if (!p->cuda_ref)
        return;
    bool pushed = false;
    if (p->cuda_ref->ctx)
        pushed = (cuCtxPushCurrent(p->cuda_ref->ctx) == CUDA_SUCCESS);
    if (p->module) {
        cuModuleUnload(p->module);
        p->module = NULL;
        p->k8 = p->k16 = p->k32 = NULL;
    }
    if (p->cuda_stream) {
        cuStreamDestroy(p->cuda_stream);
        p->cuda_stream = NULL;
    }
    if (pushed)
        cuCtxPopCurrent(NULL);
    av_buffer_unref(&p->av_hw_frames);
    av_buffer_unref(&p->av_hw_device);
    p->av_hw_frames_ctx = NULL;
    mp_cuda_ctx_ref_release(p->cuda_ref);
    p->cuda_ref = NULL;
}

static bool ensure_ctx(struct mp_filter *f, struct priv *p, struct mp_image *mpi)
{
    CUcontext ctx = mp_cuda_ctx_from_mpi(mpi);
    if (!ctx)
        return false;
    if (p->cuda_ref && p->cuda_ref->ctx == ctx)
        return true;

    reset_cuda(f, p);

    struct mp_cuda_ctx_ref *shared = mp_cuda_ctx_ref_from_mpi(mpi);
    if (shared) {
        mp_cuda_ctx_ref_acquire(shared);
        p->cuda_ref = shared;
    } else {
        p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
        if (!p->cuda_ref)
            return false;
        p->cuda_ref->magic = MP_CUDA_CTX_REF_MAGIC;
        p->cuda_ref->ctx = ctx;
        p->cuda_ref->owned = false;
        p->cuda_ref->ctx_holder = av_buffer_ref(mpi->hwctx);
        if (!p->cuda_ref->ctx_holder) {
            free(p->cuda_ref);
            p->cuda_ref = NULL;
            return false;
        }
        atomic_init(&p->cuda_ref->refs, 1);
    }

    cuCtxPushCurrent(ctx);
    CUresult cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_DEFAULT);
    bool ok = (cr == CUDA_SUCCESS);
    if (!ok)
        MP_ERR(f, "rot: cuStreamCreate failed (%d)\n", cr);
    if (ok)
        ok = ensure_kernels(f, p);
    cuCtxPopCurrent(NULL);

    if (!ok) {
        reset_cuda(f, p);
        return false;
    }
    MP_VERBOSE(f, "rot: using %s CUDA context\n",
               shared ? "upstream shared" : "borrowed hwdec");
    return true;
}

static bool ensure_frames(struct mp_filter *f, struct priv *p, int subfmt,
                          int w, int h)
{
    return mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx,
                                 imgfmt2pixfmt(subfmt), w, h,
                                 &p->av_hw_device, &p->av_hw_frames,
                                 &p->av_hw_frames_ctx);
}

// ── Launch one plane ──────────────────────────────────────────────────────

static bool launch_plane(struct mp_filter *f, CUfunction k,
                         const void *src, int src_pitch,
                         void *dst, int dst_pitch, int sw, int sh, int mode,
                         CUstream stream)
{
    int dw = (mode == 3) ? sw : sh;
    int dh = (mode == 3) ? sh : sw;
    void *args[] = {
        (void *)&src, &src_pitch,
        (void *)&dst, &dst_pitch,
        &sw, &sh, &dw, &dh, &mode
    };
    const int bx = 16, by = 16;
    CUresult cr = cuLaunchKernel(k,
        (dw + bx - 1) / bx, (dh + by - 1) / by, 1,
        bx, by, 1, 0, stream, args, NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "rot: cuLaunchKernel failed (%d)\n", cr);
        return false;
    }
    return true;
}

// ── f_process ─────────────────────────────────────────────────────────────

static void passthrough(struct mp_filter *f, struct priv *p,
                        struct mp_frame *frame, const char *why)
{
    if (!p->warned) {
        p->warned = true;
        MP_WARN(f, "rot: %s -- passthrough\n", why);
    }
    mp_pin_in_write(f->ppins[1], *frame);
}

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
        MP_ERR(f, "rot: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *mpi = frame.data;
    const int angle = p->opts->angle;
    if (angle == 0) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (angle != 90 && angle != 180 && angle != 270) {
        passthrough(f, p, &frame, "invalid angle");
        return;
    }
    if (mpi->imgfmt != IMGFMT_CUDA) {
        passthrough(f, p, &frame, "non-CUDA frame (rot must follow hwup)");
        return;
    }
    const int subfmt = mpi->params.hw_subfmt;
    if (subfmt != IMGFMT_NV12 && subfmt != IMGFMT_P010) {
        passthrough(f, p, &frame, "unsupported sub-format (need NV12/P010)");
        return;
    }
    if (mpi->w <= 0 || mpi->h <= 0 || !mpi->planes[0] || !mpi->planes[1]) {
        passthrough(f, p, &frame, "missing CUDA planes");
        return;
    }
    if (!ensure_ctx(f, p, mpi)) {
        passthrough(f, p, &frame, "no usable CUDA context");
        return;
    }

    const int bpp = (subfmt == IMGFMT_NV12) ? 1 : 2;
    const int in_w = mpi->w, in_h = mpi->h;
    const bool swap = (angle == 90 || angle == 270);
    const int out_w = swap ? in_h : in_w;
    const int out_h = swap ? in_w : in_h;
    const int mode = (angle == 90) ? 1 : (angle == 270 ? 2 : 3);

    if (!ensure_frames(f, p, subfmt, out_w, out_h)) {
        passthrough(f, p, &frame, "cannot create output frames ctx");
        return;
    }

    // 平面布局（元素宽度）：Y = bpp 字节；UV = 2*bpp 字节（NV12 的 U,V
    // 对 / P010 的两个 16-bit 样本，必须整体搬运，否则 U/V 互换）。
    const size_t y_bytes = (size_t)out_w * out_h * bpp;
    const size_t uv_bytes = y_bytes / 2;
    const size_t total = y_bytes + uv_bytes;
    const int y_elem = bpp;
    const int uv_elem = 2 * bpp;
    const int dst_y_pitch = out_w;                 // elements
    const int dst_uv_pitch = out_w * bpp / uv_elem;  // = out_w/2
    if (dst_uv_pitch != (swap ? in_h / 2 : in_w / 2)) {
        // 只可能出现在奇数宽高；按不支持的尺寸直通
        passthrough(f, p, &frame, "odd dimensions");
        return;
    }

    cuCtxPushCurrent(p->cuda_ref->ctx);
    CUdeviceptr dev = 0;
    CUresult cr = cuMemAlloc(&dev, total);
    bool ok = (cr == CUDA_SUCCESS);
    if (!ok)
        MP_ERR(f, "rot: cuMemAlloc(%zu) failed (%d)\n", total, cr);

    if (ok) {
        CUfunction ky = (bpp == 1) ? p->k8 : p->k16;
        CUfunction kuv = (bpp == 1) ? p->k16 : p->k32;
        ok = launch_plane(f, ky, mpi->planes[0], mpi->stride[0] / y_elem,
                          (void *)dev, dst_y_pitch, in_w, in_h, mode,
                          p->cuda_stream) &&
             launch_plane(f, kuv, mpi->planes[1], mpi->stride[1] / uv_elem,
                          (void *)(dev + y_bytes), dst_uv_pitch,
                          in_w / 2, in_h / 2, mode, p->cuda_stream);
    }
    if (ok) {
        cr = cuStreamSynchronize(p->cuda_stream);
        ok = (cr == CUDA_SUCCESS);
        if (!ok)
            MP_ERR(f, "rot: cuStreamSynchronize failed (%d)\n", cr);
    }
    cuCtxPopCurrent(NULL);

    if (!ok) {
        if (dev)
            cuMemFree(dev);
        passthrough(f, p, &frame, "CUDA rotation failed");
        return;
    }

    mp_cuda_ctx_ref_acquire(p->cuda_ref);
    struct mp_image *out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t *)dev, (int)total,
                                    free_cuda_buf, p->cuda_ref, 0);
    if (!out->bufs[0]) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        talloc_free(out);
        cuMemFree(dev);
        passthrough(f, p, &frame, "cannot wrap output frame");
        return;
    }

    mp_image_setfmt(out, IMGFMT_CUDA);
    mp_image_sethwfmt(out, IMGFMT_CUDA, subfmt);
    mp_image_params_guess_csp(&out->params);
    out->params.color = mpi->params.color;      // HDR 元数据
    out->params.repr  = mpi->params.repr;       // 源 levels/matrix
    out->params.chroma_location = mpi->params.chroma_location;
    out->params.rotate = mpi->params.rotate;    // 文件 rotate 元数据（叠加）
    // 90/270 交换像素宽高比，保持显示形状（p_w/p_h 是 PAR）
    out->params.p_w = swap ? mpi->params.p_h : mpi->params.p_w;
    out->params.p_h = swap ? mpi->params.p_w : mpi->params.p_h;
    out->w = out_w;
    out->h = out_h;
    out->params.w = out_w;
    out->params.h = out_h;
    out->planes[0] = (uint8_t *)dev;
    out->planes[1] = (uint8_t *)(dev + y_bytes);
    out->stride[0] = out_w * bpp;
    out->stride[1] = out_w * bpp;
    out->num_planes = 2;
    out->hwctx = av_buffer_ref(p->av_hw_frames);
    out->pts = mpi->pts;
    out->dts = mpi->dts;
    out->nominal_fps = mpi->nominal_fps;

    if (p->last_angle != angle || p->last_in_w != in_w || p->last_in_h != in_h) {
        p->last_angle = angle;
        p->last_in_w = in_w;
        p->last_in_h = in_h;
        MP_INFO(f, "rot: %d deg  %dx%d -> %dx%d\n", angle, in_w, in_h,
                out_w, out_h);
    }

    talloc_free(mpi);
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
}

// ── f_reset / f_destroy ───────────────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    // 无跨帧状态。上下文/引擎保留（与 vsr/vfg 同款：seek 不重建）。
    (void)f;
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    reset_cuda(f, p);
}

// ── vf-command rot angle N（运行期切换，不重建链）────────────────────────

static bool rot_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || !cmd->arg)
        return false;
    if (strcmp(cmd->cmd, "angle") != 0)
        return false;

    char *end = NULL;
    long v = strtol(cmd->arg, &end, 10);
    if (!end || *end || v < 0 || v > 270 || (v % 90) != 0)
        return false;
    atomic_store(&g_last_angle, (int)v);
    if (p->opts->angle != (int)v) {
        MP_INFO(f, "rot: angle %d -> %ld (vf-command)\n", p->opts->angle, v);
        p->opts->angle = (int)v;
        p->last_angle = -1;   // 下一帧重新打印尺寸
        p->warned = false;
    }
    return true;
}

// ── vf_rot_filter ─────────────────────────────────────────────────────────

static const struct mp_filter_info vf_rot_filter = {
    .name      = "rot",   // 同时是日志模块名（filters/filter.c）
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = rot_command,
};

static struct mp_filter *vf_rot_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_rot_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    // 继承最后一次运行期状态（见 g_last_angle 的注释）
    int last = atomic_load(&g_last_angle);
    if (last >= 0 && last <= 270 && (last % 90) == 0)
        p->opts->angle = last;
    p->last_angle = -1;
    return f;
}

// ── Registration ───────────────────────────────────────────────────────────

const struct mp_user_filter_entry vf_rot = {
    .desc = {
        .name          = "rot",
        .description   = "90/180/270 degree rotation on CUDA frames (zero-copy)",
        .priv_size     = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_rot_opts_def,
        .options       = vf_opts_fields,
    },
    .create = vf_rot_create,
};
