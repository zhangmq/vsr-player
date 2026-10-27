// vf_eye.c — 单眼输出（SBS/TAB 片源在 2D 环境下只看一只眼，CUDA 零拷贝）
//
// 链上位置：vf_hwup 之后、vf_rot/vf_vsr/vf_truehdr/vf_vfg 之前
// （`hwup → eye → rot → vsr → truehdr → vfg`）。输入恒为 IMGFMT_CUDA，
// hw_subfmt = NV12 或 P010；用 NVRTC 编译的 kernel 直接读/写显存平面——
// 没有 hwdownload/hwupload、没有 CPU 拷贝。
//
// eye:     off / left / right（left = 左半幅或上半幅，right = 另一半）
// layout:  sbs（左右并排）/ tab（上下并列）
// half:    0 = 全幅（源眼像素**原样**）/ 1 = 半幅（源眼沿 split 轴压了 2×）
//
// 输出尺寸（= "单眼画面"的像素尺寸，下游一律按普通 2D 帧处理）：
//   sbs + 全: (W/2, H)      sbs + 半: (W, H)    ← 半幅横向拉伸 2×
//   tab + 全: (W, H/2)      tab + 半: (W, H)    ← 半幅纵向拉伸 2×
// 也就是：全幅 = 裁切（1:1 拷贝）；半幅 = 裁切 + 沿 split 轴重采样回自然画幅。
//
// ⚠ **半幅必须真拉伸，不能只设像素宽高比（p_w/p_h）**：
//   底栏「画面比例」（mpv video-aspect-override）一旦被用户设成固定比例，
//   会覆盖 p_w/p_h 的效果 → 半幅画面被压扁；而且 VSR 的 scale=auto 按帧
//   w/h 算倍率，960×1080 会算出偏大的倍率。烘焙成标准单眼尺寸后，比例、
//   旋转、VSR auto、VFG 全都按普通 2D 视频工作，客户端不需要任何特例。
//
// 采样：Y 用双线性（全幅时 scale=1 退化为精确拷贝），UV 用最近邻
//   （4:2:0 色度、且 NV12/P010 的 U/V 是交错排布的 pair，必须整体搬运，
//   不能把 pair 当整数做插值——那会把 U/V 的进位混在一起）。
//
// 输出帧：新 cuMemAlloc（Y + UV 两区段，与 hwup/rot/vsr 同布局），hwctx 是
// **输出尺寸**的 CUDA/NV12|P010 frames ctx（mp_cuda_ensure_frames）——
// 末端的 hwdownload 按 hwctx 尺寸解析，借用源尺寸的 hwctx 会错乱/崩溃。
//
// 运行期：`vf-command eye left|right|off`、`vf-command eye stereo <sbs|tab> <full|half>`
// （不重建链；链串里也带初值，链重建后由客户端重放）。

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

struct vf_eye_opts {
    int mode;     // 0=off, 1=left(top), 2=right(bottom)
    int layout;   // 0=sbs, 1=tab
    int half;     // 0=full, 1=half
};

#define OPT_BASE_STRUCT struct vf_eye_opts

static const struct m_option vf_opts_fields[] = {
    {"mode", OPT_CHOICE(mode, {"off", 0}, {"left", 1}, {"right", 2}),
        M_RANGE(0, 2)},
    {"layout", OPT_CHOICE(layout, {"sbs", 0}, {"tab", 1}), M_RANGE(0, 1)},
    {"half", OPT_INT(half), .min = 0, .max = 1},
    {0}
};

static const struct vf_eye_opts vf_eye_opts_def = {
    .mode = 0,
    .layout = 0,
    .half = 0,
};

// 同 vf_rot：进程内"最后一次 vf-command 的状态"，滤镜实例重建时覆盖链串初值。
// 这里三个选项**都会改变输出尺寸**（模式 off↔on、layout sbs↔tab、half 0↔1），
// 不继承就会像 rot 一样形成"重建 → 尺寸变 → 再重建"的循环。
static _Atomic int g_last_mode   = -1;
static _Atomic int g_last_layout = -1;
static _Atomic int g_last_half   = -1;

// ── Filter private state ───────────────────────────────────────────────────

struct priv {
    struct vf_eye_opts *opts;

    // CUDA context 借用（与 vf_rot/vf_vsr/vf_vfg 同构，见 cuda_shared.h）
    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream cuda_stream;

    AVBufferRef *av_hw_device;
    AVBufferRef *av_hw_frames;      // CUDA/NV12|P010，**输出尺寸**
    CUcontext    av_hw_frames_ctx;

    CUmodule   module;
    CUfunction ky8, ky16;           // Y：双线性
    CUfunction kuv16, kuv32;        // UV：最近邻（pair 整体搬运）

    bool warned;
    int last_eye, last_layout, last_half, last_in_w, last_in_h;  // 日志去重
};

// ── Kernel source ─────────────────────────────────────────────────────────
// Y 双线性：8 个参数 src, src_pitch, dst, dst_pitch, sx0, sy0, sw, sh, dw, dh
// （元素单位；sx0/sy0 是源眼矩形在源平面里的左上角）。

static const char *kKernelSrc =
"extern \"C\" __global__ void eye_y_u8(const unsigned char *__restrict__ s, int sp,\n"
"    unsigned char *__restrict__ d, int dp,\n"
"    int sx0, int sy0, int sw, int sh, int dw, int dh)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    float fx = (float)sx0 + ((float)x + 0.5f) * ((float)sw / (float)dw) - 0.5f;\n"
"    float fy = (float)sy0 + ((float)y + 0.5f) * ((float)sh / (float)dh) - 0.5f;\n"
"    float ax = fx - floorf(fx), ay = fy - floorf(fy);\n"
"    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);\n"
"    int x1 = x0 + 1, y1 = y0 + 1;\n"
"    int xlo = sx0, xhi = sx0 + sw - 1, ylo = sy0, yhi = sy0 + sh - 1;\n"
"    x0 = min(max(x0, xlo), xhi); x1 = min(max(x1, xlo), xhi);\n"
"    y0 = min(max(y0, ylo), yhi); y1 = min(max(y1, ylo), yhi);\n"
"    float v0 = (1.0f - ax) * (float)s[(long)y0 * sp + x0] + ax * (float)s[(long)y0 * sp + x1];\n"
"    float v1 = (1.0f - ax) * (float)s[(long)y1 * sp + x0] + ax * (float)s[(long)y1 * sp + x1];\n"
"    float v = (1.0f - ay) * v0 + ay * v1;\n"
"    d[(long)y * dp + x] = (unsigned char)(v + 0.5f);\n"
"}\n"
"extern \"C\" __global__ void eye_y_u16(const unsigned short *__restrict__ s, int sp,\n"
"    unsigned short *__restrict__ d, int dp,\n"
"    int sx0, int sy0, int sw, int sh, int dw, int dh)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    float fx = (float)sx0 + ((float)x + 0.5f) * ((float)sw / (float)dw) - 0.5f;\n"
"    float fy = (float)sy0 + ((float)y + 0.5f) * ((float)sh / (float)dh) - 0.5f;\n"
"    float ax = fx - floorf(fx), ay = fy - floorf(fy);\n"
"    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);\n"
"    int x1 = x0 + 1, y1 = y0 + 1;\n"
"    int xlo = sx0, xhi = sx0 + sw - 1, ylo = sy0, yhi = sy0 + sh - 1;\n"
"    x0 = min(max(x0, xlo), xhi); x1 = min(max(x1, xlo), xhi);\n"
"    y0 = min(max(y0, ylo), yhi); y1 = min(max(y1, ylo), yhi);\n"
"    float v0 = (1.0f - ax) * (float)s[(long)y0 * sp + x0] + ax * (float)s[(long)y0 * sp + x1];\n"
"    float v1 = (1.0f - ax) * (float)s[(long)y1 * sp + x0] + ax * (float)s[(long)y1 * sp + x1];\n"
"    float v = (1.0f - ay) * v0 + ay * v1;\n"
"    d[(long)y * dp + x] = (unsigned short)(v + 0.5f);\n"
"}\n"
// UV 最近邻（元素 = 一个 U/V pair：NV12 2 字节、P010 4 字节）
"extern \"C\" __global__ void eye_uv_u16(const unsigned short *__restrict__ s, int sp,\n"
"    unsigned short *__restrict__ d, int dp,\n"
"    int sx0, int sy0, int sw, int sh, int dw, int dh)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    int sx = sx0 + (int)(((float)x + 0.5f) * ((float)sw / (float)dw));\n"
"    int sy = sy0 + (int)(((float)y + 0.5f) * ((float)sh / (float)dh));\n"
"    sx = min(max(sx, sx0), sx0 + sw - 1);\n"
"    sy = min(max(sy, sy0), sy0 + sh - 1);\n"
"    d[(long)y * dp + x] = s[(long)sy * sp + sx];\n"
"}\n"
"extern \"C\" __global__ void eye_uv_u32(const unsigned int *__restrict__ s, int sp,\n"
"    unsigned int *__restrict__ d, int dp,\n"
"    int sx0, int sy0, int sw, int sh, int dw, int dh)\n"
"{\n"
"    int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
"    int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
"    if (x >= dw || y >= dh) return;\n"
"    int sx = sx0 + (int)(((float)x + 0.5f) * ((float)sw / (float)dw));\n"
"    int sy = sy0 + (int)(((float)y + 0.5f) * ((float)sh / (float)dh));\n"
"    sx = min(max(sx, sx0), sx0 + sw - 1);\n"
"    sy = min(max(sy, sy0), sy0 + sh - 1);\n"
"    d[(long)y * dp + x] = s[(long)sy * sp + sx];\n"
"}\n";

// ── Frame helpers（与 vf_rot.c 同款）──────────────────────────────────────

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
    nvrtcResult res = nvrtcCreateProgram(&prog, kKernelSrc, "eye_kernel",
                                         0, NULL, NULL);
    if (res != NVRTC_SUCCESS) {
        MP_ERR(f, "eye: nvrtcCreateProgram failed: %s\n",
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
            MP_ERR(f, "eye: NVRTC compile failed:\n%s\n", log);
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
        MP_ERR(f, "eye: cuModuleLoadData failed (%d)\n", cr);
        p->module = NULL;
        return false;
    }
    if (cuModuleGetFunction(&p->ky8, p->module, "eye_y_u8") != CUDA_SUCCESS ||
        cuModuleGetFunction(&p->ky16, p->module, "eye_y_u16") != CUDA_SUCCESS ||
        cuModuleGetFunction(&p->kuv16, p->module, "eye_uv_u16") != CUDA_SUCCESS ||
        cuModuleGetFunction(&p->kuv32, p->module, "eye_uv_u32") != CUDA_SUCCESS) {
        MP_ERR(f, "eye: cuModuleGetFunction failed\n");
        cuModuleUnload(p->module);
        p->module = NULL;
        p->ky8 = p->ky16 = p->kuv16 = p->kuv32 = NULL;
        return false;
    }
    MP_INFO(f, "eye: kernels compiled (sm_%d%d)\n", major, minor);
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
        p->ky8 = p->ky16 = p->kuv16 = p->kuv32 = NULL;
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
        MP_ERR(f, "eye: cuStreamCreate failed (%d)\n", cr);
    if (ok)
        ok = ensure_kernels(f, p);
    cuCtxPopCurrent(NULL);

    if (!ok) {
        reset_cuda(f, p);
        return false;
    }
    MP_VERBOSE(f, "eye: using %s CUDA context\n",
               shared ? "upstream shared" : "borrowed hwdec");
    return true;
}

static bool ensure_frames(struct mp_filter *f, struct priv *p, int subfmt,
                          int w, int h)
{
    // 尺寸没变就复用（eye/left↔right 切换时尺寸通常不变）
    if (p->av_hw_frames && p->av_hw_frames_ctx) {
        AVHWFramesContext *fc = (AVHWFramesContext *)p->av_hw_frames->data;
        if (fc && fc->width == w && fc->height == h &&
            fc->sw_format == imgfmt2pixfmt(subfmt))
            return true;
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames_ctx = NULL;
    }
    return mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx,
                                 imgfmt2pixfmt(subfmt), w, h,
                                 &p->av_hw_device, &p->av_hw_frames,
                                 &p->av_hw_frames_ctx);
}

// ── Launch one plane ──────────────────────────────────────────────────────

static bool launch_plane(struct mp_filter *f, CUfunction k,
                         const void *src, int src_pitch,
                         void *dst, int dst_pitch,
                         int sx0, int sy0, int sw, int sh,
                         int dw, int dh, CUstream stream)
{
    void *args[] = {
        (void *)&src, &src_pitch, (void *)&dst, &dst_pitch,
        &sx0, &sy0, &sw, &sh, &dw, &dh
    };
    const int bx = 16, by = 16;
    CUresult cr = cuLaunchKernel(k,
        (dw + bx - 1) / bx, (dh + by - 1) / by, 1,
        bx, by, 1, 0, stream, args, NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "eye: cuLaunchKernel failed (%d)\n", cr);
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
        MP_WARN(f, "eye: %s -- passthrough\n", why);
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
        MP_ERR(f, "eye: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *mpi = frame.data;
    const int eye = p->opts->mode;
    if (eye == 0) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (mpi->imgfmt != IMGFMT_CUDA) {
        passthrough(f, p, &frame, "non-CUDA frame (eye must follow hwup)");
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

    const int bpp = (subfmt == IMGFMT_NV12) ? 1 : 2;
    const int in_w = mpi->w, in_h = mpi->h;
    const bool sbs = (p->opts->layout == 0);
    const bool half = p->opts->half != 0;
    const bool right = (eye == 2);

    // 源眼矩形（Y 平面元素坐标）
    const int eye_w = sbs ? in_w / 2 : in_w;
    const int eye_h = sbs ? in_h : in_h / 2;
    if ((sbs && (in_w & 1)) || (!sbs && (in_h & 1))) {
        passthrough(f, p, &frame, "odd dimensions");
        return;
    }
    const int sx0 = (sbs && right) ? in_w / 2 : 0;
    const int sy0 = (!sbs && right) ? in_h / 2 : 0;

    // 输出尺寸：半幅沿 split 轴拉回自然画幅（= 源整帧像素尺寸）
    const int out_w = half ? in_w : eye_w;
    const int out_h = half ? in_h : eye_h;

    if (!ensure_ctx(f, p, mpi)) {
        passthrough(f, p, &frame, "no usable CUDA context");
        return;
    }
    if (!ensure_frames(f, p, subfmt, out_w, out_h)) {
        passthrough(f, p, &frame, "cannot create output frames ctx");
        return;
    }

    const size_t y_bytes = (size_t)out_w * out_h * bpp;
    const size_t uv_bytes = y_bytes / 2;
    const size_t total = y_bytes + uv_bytes;
    const int y_elem = bpp;
    const int uv_elem = 2 * bpp;
    // 元素单位的 pitch（dst 是紧排的）
    const int dst_y_pitch = out_w;
    const int dst_uv_pitch = out_w / 2;

    cuCtxPushCurrent(p->cuda_ref->ctx);
    CUdeviceptr dev = 0;
    CUresult cr = cuMemAlloc(&dev, total);
    bool ok = (cr == CUDA_SUCCESS);
    if (!ok)
        MP_ERR(f, "eye: cuMemAlloc(%zu) failed (%d)\n", total, cr);

    if (ok) {
        CUfunction ky = (bpp == 1) ? p->ky8 : p->ky16;
        CUfunction kuv = (bpp == 1) ? p->kuv16 : p->kuv32;
        ok = launch_plane(f, ky, mpi->planes[0], mpi->stride[0] / y_elem,
                          (void *)dev, dst_y_pitch,
                          sx0, sy0, eye_w, eye_h, out_w, out_h,
                          p->cuda_stream) &&
             launch_plane(f, kuv, mpi->planes[1], mpi->stride[1] / uv_elem,
                          (void *)(dev + y_bytes), dst_uv_pitch,
                          sbs ? sx0 / 2 : 0, sbs ? 0 : sy0 / 2,
                          eye_w / 2, eye_h / 2, out_w / 2, out_h / 2,
                          p->cuda_stream);
    }
    if (ok) {
        cr = cuStreamSynchronize(p->cuda_stream);
        ok = (cr == CUDA_SUCCESS);
        if (!ok)
            MP_ERR(f, "eye: cuStreamSynchronize failed (%d)\n", cr);
    }
    cuCtxPopCurrent(NULL);

    if (!ok) {
        if (dev)
            cuMemFree(dev);
        passthrough(f, p, &frame, "CUDA eye extraction failed");
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
    out->params.rotate = mpi->params.rotate;    // 文件 rotate 元数据
    // 半幅已经真实拉伸过，像素宽高比保持源值（不再需要 PAR 补偿）
    out->params.p_w = mpi->params.p_w;
    out->params.p_h = mpi->params.p_h;
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

    if (p->last_eye != eye || p->last_layout != p->opts->layout ||
        p->last_half != p->opts->half ||
        p->last_in_w != in_w || p->last_in_h != in_h) {
        p->last_eye = eye;
        p->last_layout = p->opts->layout;
        p->last_half = p->opts->half;
        p->last_in_w = in_w;
        p->last_in_h = in_h;
        MP_INFO(f, "eye: %s %s %s  %dx%d -> %dx%d\n",
                right ? "right" : "left", sbs ? "sbs" : "tab",
                half ? "half" : "full", in_w, in_h, out_w, out_h);
    }

    talloc_free(mpi);
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
}

// ── f_reset / f_destroy ───────────────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    // 无跨帧状态；上下文/引擎保留（与 rot/vsr/vfg 同款：seek 不重建）。
    (void)f;
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    reset_cuda(f, p);
}

// ── vf-command（运行期切换，不重建链）────────────────────────────────────
//   eye left|right|off   （选项名是 mode，命令名仍是 eye）
//   eye stereo <sbs|tab> <full|half>

static bool eye_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || !cmd->arg)
        return false;
    if (strcmp(cmd->cmd, "eye") != 0)
        return false;

    const char *arg = cmd->arg;
    if (strncmp(arg, "stereo ", 7) == 0) {
        char layout[8] = {0}, size[8] = {0};
        if (sscanf(arg + 7, "%7s %7s", layout, size) != 2)
            return false;
        int lay = -1, half = -1;
        if (!strcmp(layout, "sbs")) lay = 0;
        else if (!strcmp(layout, "tab")) lay = 1;
        if (!strcmp(size, "full")) half = 0;
        else if (!strcmp(size, "half")) half = 1;
        if (lay < 0 || half < 0)
            return false;
        if (p->opts->layout != lay || p->opts->half != half) {
            MP_INFO(f, "eye: layout %s size %s (vf-command)\n",
                    lay == 0 ? "sbs" : "tab", half ? "half" : "full");
            p->opts->layout = lay;
            p->opts->half = half;
            atomic_store(&g_last_layout, lay);
            atomic_store(&g_last_half, half);
            p->last_eye = -1;   // 下一帧重新打印
        }
        return true;
    }

    int v = -1;
    if (!strcmp(arg, "off")) v = 0;
    else if (!strcmp(arg, "left")) v = 1;
    else if (!strcmp(arg, "right")) v = 2;
    if (v < 0)
        return false;
    if (p->opts->mode != v) {
        static const char *names[] = {"off", "left", "right"};
        MP_INFO(f, "eye: %s (vf-command)\n", names[v]);
        p->opts->mode = v;
        atomic_store(&g_last_mode, v);
        p->last_eye = -1;
        p->warned = false;
    }
    return true;
}

// ── vf_eye_filter ─────────────────────────────────────────────────────────

static const struct mp_filter_info vf_eye_filter = {
    .name      = "eye",   // 同时是日志模块名（filters/filter.c）
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = eye_command,
};

static struct mp_filter *vf_eye_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_eye_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    // 继承最后一次运行期状态（见 g_last_* 注释）
    int m = atomic_load(&g_last_mode);
    if (m >= 0 && m <= 2) p->opts->mode = m;
    int lay = atomic_load(&g_last_layout);
    if (lay >= 0 && lay <= 1) p->opts->layout = lay;
    int hf = atomic_load(&g_last_half);
    if (hf >= 0 && hf <= 1) p->opts->half = hf;
    p->last_eye = -1;
    return f;
}

// ── Registration ───────────────────────────────────────────────────────────

const struct mp_user_filter_entry vf_eye = {
    .desc = {
        .name          = "eye",
        .description   = "single-eye extraction for SBS/TAB sources (CUDA, zero-copy)",
        .priv_size     = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_eye_opts_def,
        .options       = vf_opts_fields,
    },
    .create = vf_eye_create,
};
