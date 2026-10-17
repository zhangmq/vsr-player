/*
 * vf_vfg.c -- AI frame interpolation via NVIDIA VFX SDK VideoFrameGeneration
 *
 * 输入约定：与 vf_vsr.c 相同——只处理 CUDA 帧（SW→HW 一致化由链上的
 * vf_hwup 负责）；非 CUDA 帧一律直通（不初始化引擎、不碰 CUDA）。
 * CUDA/RGBA：D2D 拷入引擎输入缓冲；CUDA/YUV：yuv_to_rgba D2D。
 * 本滤镜不假设自己前面是 VSR 还是解码器（链序 hwup → vsr → vfg 是客户端
 * 的拼法，源码不依赖它）。
 *
 * 调度（逐条对齐 vf_rife.c 的 integer_factor 分支）：倍率 K（选项
 * fps=2|3|4）——对每一对 (prev, cur) 输出 K 帧 = 插值帧 i=1..K-1 +
 * **保留源帧 cur 本身**（vs-mlrt Interleave([src, interp]) 语义）。
 * PTS 打在**内容时间**：插值帧 t_prev + (i/K)·(t_cur − t_prev)、cur 帧
 * t_cur —— 严格单调、无空洞（历史「16s desync + 持续丢帧」的修复方案）。
 * 绝不退回绝对网格（t0 + n/out_fps + llround 锚定）：源帧率不是输出网格
 * 整数倍时（如 29.97→60 比例 2.002）它会周期性重复/空洞网格点 → mpv
 * "Invalid video timestamp" + desync + 持续丢帧。
 *
 * 为什么插值不需要源帧率（2026-10-16）：上面的 PTS 是**内容相对时间**
 * t_prev + (i/K)·(t_cur − t_prev)，只依赖每对帧自己的 PTS 与倍率 K——源
 * 帧率根本不进入出帧时序。src_fps 只用于输出帧的 nominal_fps/pkt_duration
 * 元数据与状态行的 out=。因此 src_fps<=0（直播流 cur->nominal_fps 恒为 0）
 * **不构成直通理由**：原 "src-fps" 直通分支已删除（它会整条直播流直通，
 * 表现为 vfg-status: mode=passthrough reason=src-fps）。src_fps 未知时
 * 元数据保持未知（0），绝不写 1.0/0 = inf。
 *
 * 场景切换：**只依赖引擎自带检测**（AutomaticShotChangeDetectionEnabled=1，
 * SDK 1.3 能力）——本滤镜不实现第二套 MAE 检测（单一事实源；vf_rife 的
 * RIFE_SC_MAE 是 RIFE 引擎缺此能力的历史产物）。命中剪切时引擎的直通行为
 * = 复制当前帧（cur）。⚠️ 与 vf_rife.c「复制时间较近端点」（alpha>0.5 复制
 * cur、否则复制 prev）**语义不同**：rife 按 tval 在两端之间各自就近取端点，
 * VFG 的中间帧内容由引擎内部检测决定，本节点只把「FrameIndex 模式的中间帧」
 * 统一映射到 cur 侧，没有第二个判断点。
 *
 * 不做（有意，不为对齐而引入）：
 *   - adaptive/性能自动降级（PR #19 移除；cost= 只是观测量）
 *   - 非整数倍率的绝对网格路径（VFG 只做整数倍率；Timestep 模式未验证）
 *   - 目标帧率接近源帧率的直通保护（倍率语义 ≥2×，无 1.00x 情形）
 *
 * Filter options:
 *   fps:  off | 2 | 3 | 4                (default: off)  整数倍率 K
 *   mode: low | medium | high            (default: medium)
 *
 * mode **只有三档、没有 auto**（2026-10-16 决定：不要自动档）——档位是用户的显式
 * 选择，客户端总是发显式值，滤镜侧不再有任何"自动挑档"的语义。
 *
 * 「按分辨率猜档位」为什么废弃（实测，取代旧的「输入高度 ≥2160 → high」）：
 * 该规则在后置链序（hwup → vsr → vfg）下会打反效果——**非 4K 源经 VSR 放大
 * 到 4K**（1080p + vsr:scale=2，即 4K 屏全屏）使 VFG 输入高度正好踩中 2160
 * → 自动选 HIGH，而这一档 VSR 正在跑、整链预算最紧。播放器内实测
 * （1080p × scale=2、K=2、4K 输出，两次都 0 丢帧）：
 *   HIGH   : init 3840×2160, VFG cost 13.2ms, 帧时间 13.99/24.06/27.18 ms (中位/p90/最大)
 *   MEDIUM : init 3840×2160, VFG cost  3.8ms, 帧时间 16.20/16.48/18.62 ms
 * 即 HIGH 在 4K 上贵 **3.5×**、帧时间尾部 p90 从 16.5ms 恶化到 24.1ms；另一次
 * 同配置运行还累积了 9 个丢帧（运行间可变）。而 MEDIUM 在**原生 4K** 素材
 * （3840×1600，VSR 直通）上实测 cost=2.7ms / 48fps / 0 丢帧——完全够用。
 * HIGH 相对 MEDIUM 的画质收益也很小：4K 插值帧的 mid/blend 高频比 MEDIUM
 * 0.918 vs HIGH 0.956，且**目检（1:1 裁剪）未见可见软化**。
 * 更根本的理由：**按分辨率猜档位属于「节点内依据局部信息做全局决策」**，与
 * PR #19 移除的 RIFE 性能自动降级同源。
 * ⚠️ **HIGH 并未被移除**——仍可用 mode=high 显式启用（--vf 选项 /
 *    `vf-command vfg mode high` / RPC）。
 */

#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "common/common.h"
#include "mpv/client.h"   // struct mpv_node（自定义 option type 的 set/get）

#include <cuda.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>

#include "vfg_proc.h"
#include "cuda_shared.h"
#include "yuv_to_rgba.h"

// vfg-status 节奏（输出帧）
#define VFG_STATUS_EVERY 30
// 选项层暴露的倍率范围（冻结接口：fps=off|2|3|4）。与 SDK 的
// VFG_MULT_MIN..MAX（2..8）刻意分开：SDK 能跑 5..8，但接口不暴露——越界
// 在解析期清晰报错，而不是静默按 5× 跑（实测过：先按 2..8 写，fps=5
// 被接受并真的按 k=5 输出 150fps）。
#define VFG_FPS_OPT_MIN 2
#define VFG_FPS_OPT_MAX 4

static double vfg_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void f_reset(struct mp_filter *f);   // 定义在调度段之后（vf-command 需要）

// ── Options ────────────────────────────────────────────────────────────────

struct vf_vfg_opts {
    int fps;    // -1=off, 2/3/4=整数倍率 K
    int mode;   // 0=low, 1=medium, 2=high（默认 1=medium，无 auto）
};

#define OPT_BASE_STRUCT struct vf_vfg_opts

// fps 选项类型（镜像 vf_rife.c 的 rife_fps_type）："off"→-1，整数只接受
// 2..4（SDK 的 FrameMultiplier 支持 2..8，冻结接口只暴露 2/3/4），
// 越界一律 M_OPT_INVALID 并给出清晰信息。
static int vfg_fps_parse(struct mp_log *log, const m_option_t *opt,
                         struct bstr name, struct bstr param, void *dst)
{
    int v;
    if (bstr_equals0(param, "off")) {
        v = -1;
    } else {
        if (param.len == 0)
            return M_OPT_MISSING_PARAM;
        struct bstr rest;
        long long n = bstrtoll(param, &rest, 10);
        if (rest.len || n < VFG_FPS_OPT_MIN || n > VFG_FPS_OPT_MAX) {
            mp_err(log, "The %.*s option must be off or an integer %d..%d: %.*s\n",
                   BSTR_P(name), VFG_FPS_OPT_MIN, VFG_FPS_OPT_MAX, BSTR_P(param));
            return M_OPT_INVALID;
        }
        v = (int)n;
    }
    if (dst)
        *(int *)dst = v;
    return 1;
}

static char *vfg_fps_print(const m_option_t *opt, const void *val)
{
    return talloc_asprintf(NULL, "%d", *(const int *)val);
}

static void vfg_fps_copy(const m_option_t *opt, void *dst, const void *src)
{
    if (dst && src)
        memcpy(dst, src, sizeof(int));
}

static int vfg_fps_set(const m_option_t *opt, void *dst, struct mpv_node *src)
{
    int v;
    if (src->format == MPV_FORMAT_INT64)
        v = (int)src->u.int64;
    else if (src->format == MPV_FORMAT_DOUBLE)
        v = (int)src->u.double_;
    else
        return M_OPT_INVALID;
    if (v != -1 && (v < VFG_FPS_OPT_MIN || v > VFG_FPS_OPT_MAX))
        return M_OPT_OUT_OF_RANGE;
    *(int *)dst = v;
    return 1;
}

static int vfg_fps_get(const m_option_t *opt, void *ta_parent,
                       struct mpv_node *dst, void *src)
{
    dst->format = MPV_FORMAT_INT64;
    dst->u.int64 = *(const int *)src;
    return 1;
}

static bool vfg_fps_equal(const m_option_t *opt, void *a, void *b)
{
    return memcmp(a, b, sizeof(int)) == 0;
}

static const m_option_type_t vfg_fps_type = {
    .name  = "vfg-fps",
    .size  = sizeof(int),
    .parse = vfg_fps_parse,
    .print = vfg_fps_print,
    .copy  = vfg_fps_copy,
    .set   = vfg_fps_set,
    .get   = vfg_fps_get,
    .equal = vfg_fps_equal,
};

#define OPT_VFG_FPS(field) \
    OPT_TYPED_FIELD(vfg_fps_type, int, field)

static const struct m_option vf_opts_fields[] = {
    {"fps",  OPT_VFG_FPS(fps)},
    {"mode", OPT_CHOICE(mode,
        {"low", 0}, {"medium", 1}, {"high", 2}),
        M_RANGE(0, 2)},
    {0}
};

static const struct vf_vfg_opts vf_vfg_opts_def = {
    .fps  = -1,
    .mode = VFG_MODE_MEDIUM,
};

// ── Filter private state ───────────────────────────────────────────────────

/// CUDA context ownership — 与 vf_vsr.c 完全同构（共享结构 cuda_shared.h）：
///   - 借上游共享结构（hwup/vsr 输出帧的 bufs[0] opaque）
///   - hwdec 帧（opaque 非本链共享结构）：自建借者结构 + ctx_holder
///   - 软解帧：直通（契约：SW→HW 由 vf_hwup 负责）
struct priv {
    struct vf_vfg_opts *opts;

    struct mp_cuda_ctx_ref *cuda_ref;
    CUstream  cuda_stream;
    bool      cuda_init_done;

    AVBufferRef *av_hw_device;      // CUDA device ctx（输出 RGBA frames 用）
    AVBufferRef *av_hw_frames;      // CUDA/RGBA frames ctx
    CUcontext    av_hw_frames_ctx;  // ctx av_hw_frames was created on

    struct vfg_context vfg;
    bool vfg_configured;
    int  vfg_mode;                  // 引擎当前解析后的 Mode（enum vfg_mode）

    struct yuv_to_rgba yuv_conv;    // CUDA/YUV → RGB D2D
    bool  yuv_conv_ready;
    int   yuv_conv_bd;
    int   yuv_conv_bits;            // 输出位深（8 RGBA U8 / 10 RGB10A2）
    enum yuv_matrix yuv_conv_matrix;
    enum yuv_range  yuv_conv_range;

    CUdeviceptr out_buf;            // 每个输出帧一块，交出后归 0
    size_t      out_buf_size;
    int         out_w, out_h, out_pitch;

    int    video_w, video_h;        // 引擎/输入尺寸（VFG 输出同尺寸）
    double src_fps;                 // 输入帧率（nominal_fps）
    int    k;                       // 当前倍率（= opts->fps，2..4）

    // 调度状态：一个 pair 跨多次 process() 调用展开
    bool   have_prev;               // in_img[0] 已有 prev（内容时间 prev_pts）
    double prev_pts;
    struct mp_image *pending_cur;   // 展开中的 cur（拥有权在本滤镜）
    double cur_pts;
    int    emit_i;                  // 下一次输出：1..K-1 插值，K=cur 副本

    bool passthrough;
    char pt_reason[32];
    bool degrade_warned;            // 引擎失败直通只警告一次
    bool engine_failed;             // 引擎一次性降级（不再每帧重试）
    bool sw_warned;

    int    frame_count;
    int    status_frames;           // active 分支输出计数（每 30 帧出状态行）
    double status_cost_ms;
    int    status_emit;             // passthrough 分支计数
};

// ── Status line (MSGL_STATUS, 每 30 输出帧) ───────────────────────────────
// 客户端按 "vfg-status:" 前缀提取后拼进 OSD。格式冻结：
//   active     : vfg-status: mode=active src=%.2f k=%d out=%.2f cost=%.1fms
//   passthrough: vfg-status: mode=passthrough reason=<原因>
// cost = 本节点**每输出帧**的处理耗时（含流同步；观测量，不参与决策）。

static void vfg_status(struct mp_filter *f, struct priv *p)
{
    if (p->passthrough) {
        mp_msg(f->log, MSGL_STATUS, "vfg-status: mode=passthrough reason=%s\n",
               p->pt_reason);
        return;
    }
    // src_fps 未知（<=0，直播流 nominal_fps=0）→ out 打 0.00：格式冻结为
    // %.2f（客户端 OSD 原样显示、如有数值消费者也能解析），且 src=0.00 已
    // 表达"源帧率未知"，不会像 inf/nan 那样骗人；不改成 "?" 以免破坏字段
    // 语法（字段名/顺序/数值格式都不动）。
    double out_fps = p->src_fps > 0 ? p->k * p->src_fps : 0.0;
    mp_msg(f->log, MSGL_STATUS,
           "vfg-status: mode=active src=%.2f k=%d out=%.2f cost=%.1fms\n",
           p->src_fps, p->k, out_fps,
           p->status_frames > 0 ? p->status_cost_ms / p->status_frames : 0.0);
    p->status_frames = 0;
    p->status_cost_ms = 0;
}

static const char *vfg_mode_name(int mode)
{
    switch (mode) {
    case VFG_MODE_LOW:    return "low";
    case VFG_MODE_MEDIUM: return "medium";
    case VFG_MODE_HIGH:   return "high";
    default:              return "?";
    }
}

// ── CUDA context setup（vf_vsr.c ensure_cuda 同构）───────────────────────
// 单 context 设计：全链共用一个驱动 context（多 context 在 GPU 上时间切片，
// 与其它引擎并发时挂死设备）。来源与所有权见 cuda_shared.h。

static bool ensure_cuda(struct mp_filter *f, struct priv *p,
                        struct mp_image *mpi)
{
    CUcontext borrow = mp_cuda_ctx_from_mpi(mpi);  // NULL = SW frame

    if (p->cuda_ref) {
        if (p->cuda_ref->ctx == borrow)
            return true;
    }

    CUresult cr = cuInit(0);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "vfg: cuInit failed (%d)\n", cr);
        return false;
    }

    // 切 hwdec（软解↔硬解）会换上游 CUDA context：引擎、yuv_to_rgba 的
    // NVRTC module、流、out_buf 全是 context 绑定的，必须在**旧 context 内**
    // 作废，否则复用旧 kernel 句柄 → cuLaunchKernel 400 → 本滤镜被 mpv 禁用
    // （2026-10-17 实测："切软解/切回硬解都不再插帧"的根因）。
    if (p->cuda_ref) {
        CUcontext old = p->cuda_ref->ctx;
        if (old) cuCtxPushCurrent(old);
        if (p->vfg_configured) {
            vfg_destroy(&p->vfg);
            p->vfg_configured = false;
        }
        if (p->yuv_conv_ready) {
            yuv_to_rgba_destroy(&p->yuv_conv);
            p->yuv_conv_ready = false;
        }
        if (p->out_buf) {
            cuMemFree(p->out_buf);
            p->out_buf = 0;
            p->out_buf_size = 0;
        }
        if (p->cuda_init_done && p->cuda_stream) {
            cuStreamDestroy(p->cuda_stream);
            p->cuda_stream = NULL;
            p->cuda_init_done = false;
        }
        if (old) cuCtxPopCurrent(NULL);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        MP_VERBOSE(f, "vfg: CUDA context changed — engine/converter reset\n");
    }

    struct mp_cuda_ctx_ref *shared = mp_cuda_ctx_ref_from_mpi(mpi);
    if (shared) {
        mp_cuda_ctx_ref_acquire(shared);
        p->cuda_ref = shared;
        MP_VERBOSE(f, "vfg: sharing upstream CUDA context\n");
    } else if (borrow) {
        p->cuda_ref = calloc(1, sizeof(*p->cuda_ref));
        p->cuda_ref->magic = MP_CUDA_CTX_REF_MAGIC;
        p->cuda_ref->ctx = borrow;
        p->cuda_ref->owned = false;
        p->cuda_ref->ctx_holder = av_buffer_ref(mpi->hwctx);
        if (!p->cuda_ref->ctx_holder) {
            MP_ERR(f, "vfg: failed to hold hwdec frame ref\n");
            free(p->cuda_ref);
            p->cuda_ref = NULL;
            return false;
        }
        atomic_init(&p->cuda_ref->refs, 1);
        MP_VERBOSE(f, "vfg: sharing hwdec CUDA context\n");
    } else {
        // 软解帧已在直通分支返回；走到这里说明上游状态异常——不自建
        // context（vf_vsr 的历史教训：每帧重建 context 使引擎缓冲失效）。
        MP_ERR(f, "vfg: no CUDA context from input (software frame past passthrough)\n");
        return false;
    }

    cuCtxPushCurrent(p->cuda_ref->ctx);
    cr = cuStreamCreate(&p->cuda_stream, CU_STREAM_NON_BLOCKING);
    cuCtxPopCurrent(NULL);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "vfg: cuStreamCreate failed (%d)\n", cr);
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
        return false;
    }

    p->cuda_init_done = true;
    return true;
}

// ── yuv→rgba 转换器生命周期（vf_vsr.c ensure_yuv_converter 同构）────────

static bool ensure_yuv_converter(struct mp_filter *f, struct priv *p,
                                 int bit_depth, enum yuv_matrix matrix,
                                 enum yuv_range range, int out_bits)
{
    if (p->yuv_conv_ready &&
        p->yuv_conv_bd == bit_depth &&
        p->yuv_conv_bits == out_bits &&
        p->yuv_conv_matrix == matrix &&
        p->yuv_conv_range == range)
        return true;

    if (p->yuv_conv_ready)
        yuv_to_rgba_destroy(&p->yuv_conv);

    if (!yuv_to_rgba_init(&p->yuv_conv, bit_depth, matrix, range, out_bits)) {
        MP_ERR(f, "vfg: yuv_to_rgba_init failed (bd=%d out=%d)\n",
               bit_depth, out_bits);
        p->yuv_conv_ready = false;
        return false;
    }
    p->yuv_conv_bd = bit_depth;
    p->yuv_conv_bits = out_bits;
    p->yuv_conv_matrix = matrix;
    p->yuv_conv_range = range;
    p->yuv_conv_ready = true;
    return true;
}

// ── VFG init / reconfigure ─────────────────────────────────────────────────

static bool ensure_vfg(struct mp_filter *f, struct priv *p, int in_w, int in_h,
                       int bits)
{
    // 档位 = 用户显式选择（无 auto；废弃理由与实测见文件头），默认 medium。
    int mode = p->opts->mode;

    if (p->vfg_configured) {
        // 尺寸/Mode/stream 任一变化都要重建：SDK 对 Mode 与 InputWidth/
        // InputHeight 都要求重新 Load（官方 nvvfx 包装标 requires_reload；
        // 探针只在 Load 前设过 Mode，之后改是否生效未验证）。stream 随
        // ensure_cuda 重绑而变化时同理（CudaStream 在 Load 时绑定）。
        bool same = (p->vfg.in_w == in_w && p->vfg.in_h == in_h &&
                     p->vfg.bits == bits &&
                     p->vfg_mode == mode && p->vfg.stream == p->cuda_stream);
        if (same) {
            // FrameMultiplier 无需重建（每次 Run 前 SetU32）
            if (p->vfg.multiplier != p->k)
                vfg_set_multiplier(&p->vfg, p->k);
            return true;
        }
        MP_INFO(f, "vfg: reconfig %dx%d mode=%s bits=%d -> %dx%d mode=%s bits=%d\n",
                p->vfg.in_w, p->vfg.in_h, vfg_mode_name(p->vfg_mode), p->vfg.bits,
                in_w, in_h, vfg_mode_name(mode), bits);
        vfg_destroy(&p->vfg);
        p->vfg_configured = false;
    }

    MP_INFO(f, "vfg: init %dx%d mode=%s k=%d bits=%d\n", in_w, in_h,
            vfg_mode_name(mode), p->k, bits);
    if (!vfg_init(&p->vfg, in_w, in_h, mode, p->k, bits, p->cuda_stream)) {
        MP_ERR(f, "vfg: init failed (%dx%d mode=%s k=%d)\n",
               in_w, in_h, vfg_mode_name(mode), p->k);
        return false;
    }
    p->vfg_mode = mode;
    p->vfg_configured = true;
    MP_VERBOSE(f, "vfg: engine ready mode=%s\n", vfg_mode_name(mode));
    return true;
}

// ── Output buffer（vf_vsr.c ensure_out_buf 同构：交出后置 0，下帧重分配）─

static bool ensure_out_buf(struct mp_filter *f, struct priv *p,
                           int out_w, int out_h, int out_pitch)
{
    size_t needed = (size_t)out_pitch * out_h;
    if (p->out_buf && p->out_buf_size >= needed) {
        p->out_w     = out_w;
        p->out_h     = out_h;
        p->out_pitch = out_pitch;
        return true;
    }
    if (p->out_buf) {
        cuMemFree(p->out_buf);
        p->out_buf = 0;
        p->out_buf_size = 0;
    }
    CUresult cr = cuMemAlloc(&p->out_buf, needed);
    if (cr != CUDA_SUCCESS) {
        MP_ERR(f, "vfg: cuMemAlloc(out %zu) failed (%d)\n", needed, cr);
        return false;
    }
    p->out_buf_size = needed;
    p->out_w     = out_w;
    p->out_h     = out_h;
    p->out_pitch = out_pitch;
    return true;
}

// ── AVBufferRef free callback / mp_image dtor（vf_vsr.c 同款）─────────────

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
    // 不做流同步：缓冲只在消费端（VO 的 map 拷贝 / 下游滤镜）替换后才释放，
    // 本滤镜每帧的流同步已把拷贝排干；对共享 context 的 stream 0 同步会等
    // 渲染线程（vf_vsr.c free_cuda_buf 同款理由）。
    cuMemFree((CUdeviceptr)data);
    if (r && r->ctx) cuCtxPopCurrent(NULL);
    if (r)
        mp_cuda_ctx_ref_release(r);
}

// ── 输出帧构造（vf_vsr.c 输出段 / vf_rife.c rife_make_output 同构）──────
// 调用方须已 cuCtxPushCurrent；p->out_buf 的拥有权移交给返回帧。

static struct mp_image *vfg_make_output(struct mp_filter *f, struct priv *p,
                                        const struct mp_image *src, double pts,
                                        double out_fps)
{
    const int out_bits = p->vfg.bits;
    // sw_format 恒 RGBA（FFmpeg CUDA hwframes 不支持 x2rgb10，见
    // vf_vsr.c ensure_av_hw_frames 的说明）；10-bit 由 hw_subfmt 承载。
    if (!mp_cuda_ensure_frames(f->log, p->cuda_ref->ctx, AV_PIX_FMT_RGBA,
                               p->out_w, p->out_h,
                               &p->av_hw_device, &p->av_hw_frames,
                               &p->av_hw_frames_ctx)) {
        MP_ERR(f, "vfg: CUDA/4Bpp hw_frames_ctx failed\n");
        return NULL;
    }
    mp_cuda_ctx_ref_acquire(p->cuda_ref);
    struct mp_image *out = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(out, mp_image_dtor);
    out->bufs[0] = av_buffer_create((uint8_t *)p->out_buf, (int)p->out_buf_size,
                                    free_cuda_buf, p->cuda_ref, 0);
    if (!out->bufs[0]) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        talloc_free(out);
        return NULL;
    }
    mp_image_setfmt(out, IMGFMT_CUDA);
    mp_image_sethwfmt(out, IMGFMT_CUDA, vfx_imgfmt_from_bits(out_bits));
    mp_image_params_guess_csp(&out->params);
    out->params.color = src->params.color;   // HDR 元数据（vf_vsr 同款）
    out->params.p_w = src->params.p_w;       // 像素宽高比（aspect override）
    out->params.p_h = src->params.p_h;
    out->w = p->out_w;
    out->h = p->out_h;
    out->params.w = p->out_w;
    out->params.h = p->out_h;
    out->planes[0] = (uint8_t *)p->out_buf;
    out->stride[0] = p->out_pitch;
    out->num_planes = 1;
    out->hwctx = av_buffer_ref(p->av_hw_frames);
    out->pts         = pts;
    out->dts         = src->dts;
    // src_fps 未知（直播流 nominal_fps=0）→ 元数据保持未知：绝不写
    // 1.0/0 = inf。出帧时序不依赖 nominal_fps（PTS 是内容相对时间）。
    out->nominal_fps  = out_fps > 0 ? out_fps : src->nominal_fps;
    out->pkt_duration = out->nominal_fps > 0 ? 1.0 / out->nominal_fps : 0;
    p->out_buf = 0;              // 拥有权移交
    p->out_buf_size = 0;
    return out;
}

// ── D2D 拷贝（本滤镜的流）────────────────────────────────────────────────

static bool copy_d2d(struct mp_filter *f, struct priv *p,
                     void *dst, size_t dst_pitch,
                     const void *src, size_t src_pitch, int w, int h)
{
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice     = (CUdeviceptr)src;
    copy.srcPitch      = src_pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice     = (CUdeviceptr)dst;
    copy.dstPitch      = dst_pitch;
    copy.WidthInBytes  = (size_t)w * 4;
    copy.Height        = (size_t)h;
    if (cuMemcpy2DAsync(&copy, p->cuda_stream) != CUDA_SUCCESS) {
        MP_ERR(f, "vfg: cuMemcpy2DAsync failed\n");
        return false;
    }
    return true;
}

// ── 输入帧 → 引擎输入缓冲（CUDA/YUV 转换 或 CUDA/RGBA D2D）─────────────
// 与 vf_vsr.c:677-710 的 shim 同构（idx=1 即 SrcImage1）。

static bool stage_input(struct mp_filter *f, struct priv *p,
                        struct mp_image *cur, int idx)
{
    if (!vfx_imgfmt_is_rgb(cur->params.hw_subfmt)) {
        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(cur->params.hw_subfmt);
        int bd = desc.bpp[0];
        enum yuv_matrix mx = matrix_from_repr(&cur->params);
        enum yuv_range  rn = range_from_levels(cur->params.repr.levels);
        if (!ensure_yuv_converter(f, p, bd, mx, rn, p->vfg.bits))
            return false;
        if (!yuv_to_rgba_convert(&p->yuv_conv,
                                 cur->planes[0], cur->stride[0],
                                 cur->planes[1], cur->stride[1],
                                 p->video_w, p->video_h,
                                 p->vfg.in_pixels[idx],
                                 p->vfg.in_pitch[idx],
                                 p->cuda_stream)) {
            MP_ERR(f, "vfg: yuv_to_rgba_convert failed\n");
            return false;
        }
        return true;
    }
    return copy_d2d(f, p, p->vfg.in_pixels[idx], (size_t)p->vfg.in_pitch[idx],
                    cur->planes[0], (size_t)cur->stride[0],
                    p->video_w, p->video_h);
}

// ── 状态记账（active 分支：每输出帧一次）────────────────────────────────

static void vfg_account(struct mp_filter *f, struct priv *p, double t0)
{
    p->status_cost_ms += vfg_now_ms() - t0;
    p->status_frames++;
    if (p->status_frames >= VFG_STATUS_EVERY)
        vfg_status(f, p);
}

// ── 输出 pair 的一张帧 ───────────────────────────────────────────────────
// i=1..K-1 走 VFG Run（内容时间 t_prev + (i/K)·Δ）；i=K 输出 cur 本身
// （内容取 in_img[1] 的 RGBA staging）。调用方须已 cuCtxPushCurrent 且
// pending_cur / emit_i 就绪。每调用输出**恰好一帧**。

static bool emit_pair_output(struct mp_filter *f, struct priv *p)
{
    struct mp_image *cur = p->pending_cur;
    double t_prev = p->prev_pts;
    double t_cur  = p->cur_pts;
    int k = p->k;
    double out_fps = k * p->src_fps;
    int i = p->emit_i;
    double t0 = vfg_now_ms();
    const void *src;
    size_t src_pitch;
    double pts;

    if (i < k) {
        void *vfg_out = NULL;
        int vfg_pitch = 0;
        if (!vfg_process(&p->vfg, i, &vfg_out, &vfg_pitch))
            return false;
        src = vfg_out;
        src_pitch = (size_t)vfg_pitch;
        // 内容时间：恒 t = i/K（vs-mlrt Interleave 语义——VFR 源上也不会
        // 摆到极端 tval）
        double tval = (double)i / k;
        pts = t_prev + (t_cur - t_prev) * tval;
    } else {
        // cur 本身（内容在 in_img[1] 的 RGBA staging，已是 full-range 转换
        // 结果——不用源 YUV 平面，避免两种色彩标记交替导致闪烁，
        // vf_rife.c rife_make_copy 同款理由）
        src = p->vfg.in_pixels[1];
        src_pitch = (size_t)p->vfg.in_pitch[1];
        pts = t_cur;
    }

    if (!ensure_out_buf(f, p, p->video_w, p->video_h, (int)src_pitch))
        return false;
    if (!copy_d2d(f, p, (void *)p->out_buf, (size_t)p->out_pitch,
                  src, src_pitch, p->video_w, p->video_h))
        return false;
    // 本帧 GPU 工作完成边界（vf_vsr 同款：下游/VO 读之前必须拷贝完成）
    cuStreamSynchronize(p->cuda_stream);

    struct mp_image *out = vfg_make_output(f, p, cur, pts, out_fps);
    if (!out)
        return false;

    MP_DBG(f, "vfg: OUT pts=%.4f i=%d/%d %s\n", pts, i, k,
           i < k ? "[mid]" : "[copy]");
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
    vfg_account(f, p, t0);

    if (i >= k) {
        // pair 完成：cur 成为下一 pair 的 prev（内容已在 in_img[1]，下一
        // pair 起点搬到 in_img[0]）；释放输入帧（不长期占住解码 surface）
        p->prev_pts = t_cur;
        p->have_prev = true;
        talloc_free(p->pending_cur);
        p->pending_cur = NULL;
    } else {
        p->emit_i = i + 1;
    }
    return true;
}

// ── f_process ──────────────────────────────────────────────────────────────

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    // 输出 pin 需要数据时才工作（vf_rife 的 process 入口同款）：本滤镜把
    // 一个 pair 跨多次调用展开，展开期间不能再读输入。
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    // ── 1. 展开中的 pair：每次调用输出一帧 ─────────────────────────────
    if (p->pending_cur) {
        cuCtxPushCurrent(p->cuda_ref->ctx);
        bool ok = emit_pair_output(f, p);
        cuCtxPopCurrent(NULL);
        if (!ok) {
            talloc_free(p->pending_cur);
            p->pending_cur = NULL;
            p->have_prev = false;
            mp_filter_internal_mark_failed(f);
        }
        return;
    }

    // ── 2. 读入下一帧 ───────────────────────────────────────────────────
    if (!mp_pin_out_request_data(f->ppins[0]))
        return;
    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_NONE)
        return;
    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO) {
        MP_ERR(f, "vfg: non-video frame received\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_image *cur = frame.data;
    p->frame_count++;

    int in_w = cur->w, in_h = cur->h;
    if (cur->nominal_fps > 0)
        p->src_fps = cur->nominal_fps;

    // ── 3. 直通判定（逐帧重估：vf-command 热更新下一帧即生效）──────────
    bool sw_input = (cur->imgfmt != IMGFMT_CUDA);
    if (sw_input && !p->sw_warned) {
        MP_WARN(f, "vfg: software frames — passthrough (prepend @hwup for VFG)\n");
        p->sw_warned = true;
    }

    // 直通判定（逐帧重估）。**不因 src_fps<=0 直通**（原 "src-fps" 分支已
    // 删）：插值 PTS 只用内容相对时间，只依赖倍率 K——见文件头"为什么插值
    // 不需要源帧率"。直播流 nominal_fps=0 不再降级为直通。
    const char *reason = NULL;
    if (p->opts->fps == -1)                          reason = "off";
    else if (p->opts->fps < VFG_MULT_MIN ||
             p->opts->fps > VFG_MULT_MAX)            reason = "k-range";
    else if (sw_input)                               reason = "sw";
    else if (p->engine_failed)                       reason = "engine";
    else if (cur->pts == MP_NOPTS_VALUE && !p->have_prev)
                                                     reason = "no-pts";

    if (reason) {
        p->passthrough = true;
        snprintf(p->pt_reason, sizeof(p->pt_reason), "%s", reason);
        if (p->vfg_configured) {
            cuCtxPushCurrent(p->cuda_ref->ctx);
            vfg_destroy(&p->vfg);
            cuCtxPopCurrent(NULL);
            p->vfg_configured = false;
            MP_INFO(f, "vfg: switching to passthrough (%s)\n", reason);
        }
        p->have_prev = false;   // 调度连续性作废
        if (++p->status_emit >= VFG_STATUS_EVERY) {
            p->status_emit = 0;
            vfg_status(f, p);
        }
        mp_pin_in_write(f->ppins[1], frame);   // 直通：原帧不动
        return;
    }

    p->passthrough = false;
    p->k = p->opts->fps;

    // 重复/倒序/缺 PTS：丢弃 cur、保留 prev（vf_rife 同款）
    double t_cur = cur->pts;
    if (p->have_prev && (t_cur == MP_NOPTS_VALUE || t_cur <= p->prev_pts)) {
        MP_DBG(f, "vfg: drop frame pts=%.3f (prev=%.3f)\n", t_cur, p->prev_pts);
        talloc_free(cur);
        return;
    }

    // ── 4. CUDA context + 引擎（懒初始化）───────────────────────────────
    if (!ensure_cuda(f, p, cur)) {
        talloc_free(cur);
        mp_filter_internal_mark_failed(f);
        return;
    }
    cuCtxPushCurrent(p->cuda_ref->ctx);

    // 通路位深：源 subfmt 已是 10-bit 打包 RGB（上游 vsr）或源位深 >8
    //（P010/P016）→ 10-bit 通路；否则维持 8-bit 历史行为。
    int bits = vfx_bits_from_imgfmt(cur->params.hw_subfmt);
    if (!ensure_vfg(f, p, in_w, in_h, bits)) {
        cuCtxPopCurrent(NULL);
        // 引擎不可用 → 一次性直通降级（不再每帧重试；f_reset 后重试）
        if (!p->degrade_warned) {
            MP_WARN(f, "vfg: engine init failed — passthrough\n");
            p->degrade_warned = true;
        }
        p->engine_failed = true;
        p->passthrough = true;
        snprintf(p->pt_reason, sizeof(p->pt_reason), "engine");
        p->have_prev = false;
        if (++p->status_emit >= VFG_STATUS_EVERY) {
            p->status_emit = 0;
            vfg_status(f, p);
        }
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    p->video_w = in_w;
    p->video_h = in_h;
    double out_fps = p->k * p->src_fps;

    // ── 5. 首帧：直接输出（源帧保留语义的起点；内容即下一 pair 的 prev）─
    if (!p->have_prev) {
        double t0 = vfg_now_ms();
        if (!stage_input(f, p, cur, 1)) {
            cuCtxPopCurrent(NULL);
            talloc_free(cur);
            mp_filter_internal_mark_failed(f);
            return;
        }
        p->prev_pts = t_cur;
        p->have_prev = true;
        // 输出帧率 = K×源帧率（与后续输出帧同构；vf_rife 首帧同款）。
        // src_fps 未知时保持源帧的值，绝不写 1.0/0 = inf。
        if (out_fps > 0) {
            cur->nominal_fps = out_fps;
            cur->pkt_duration = 1.0 / out_fps;
        }
        MP_DBG(f, "vfg: first frame pts=%.4f kept (%dx%d k=%d mode=%s)\n",
               t_cur, in_w, in_h, p->k, vfg_mode_name(p->vfg_mode));
        vfg_account(f, p, t0);
        cuCtxPopCurrent(NULL);
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // ── 6. pair 起点：prev 搬进 in_img[0]、cur 进 in_img[1] ─────────────
    // SetImage 只在 Load 前调用一次（探针序列），SrcImage0/SrcImage1 与缓冲
    // 的绑定固定；因此靠两次 D2D 维持「SrcImage0=prev / SrcImage1=cur」的
    // 角色不变（in_img[1] 里是上一帧的 cur，即本 pair 的 prev）。
    if (!copy_d2d(f, p, p->vfg.in_pixels[0], (size_t)p->vfg.in_pitch[0],
                  p->vfg.in_pixels[1], (size_t)p->vfg.in_pitch[1],
                  p->video_w, p->video_h) ||
        !stage_input(f, p, cur, 1)) {
        cuCtxPopCurrent(NULL);
        talloc_free(cur);
        mp_filter_internal_mark_failed(f);
        return;
    }

    p->pending_cur = cur;   // 拥有权自 pin frame 转交
    p->cur_pts = t_cur;
    p->emit_i = 1;

    bool ok = emit_pair_output(f, p);
    cuCtxPopCurrent(NULL);
    if (!ok) {
        talloc_free(p->pending_cur);
        p->pending_cur = NULL;
        p->have_prev = false;
        mp_filter_internal_mark_failed(f);
    }
}

// ── vfg_command（vf-command vfg fps X / mode Y；照 vf_rife.c rife_command）─

static bool parse_fps_arg(const char *arg, int *out)
{
    if (!strcmp(arg, "off")) { *out = -1; return true; }
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (!end || *end || v < VFG_FPS_OPT_MIN || v > VFG_FPS_OPT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_mode_arg(const char *arg, int *out)
{
    if (!strcmp(arg, "low"))         *out = 0;
    else if (!strcmp(arg, "medium")) *out = 1;
    else if (!strcmp(arg, "high"))   *out = 2;
    else *out = atoi(arg);
    return *out >= 0 && *out <= 2;   // 无 auto：非法值（含 "auto"）一律拒绝
}

static bool vfg_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || !cmd->arg)
        return false;
    if (strcmp(cmd->cmd, "fps") == 0) {
        int v;
        if (!parse_fps_arg(cmd->arg, &v))
            return false;
        if (p->opts->fps != v) {
            MP_INFO(f, "vfg: fps %d -> %d (vf-command)\n", p->opts->fps, v);
            p->opts->fps = v;
            // 连续性破坏：重置调度状态（引擎保留），下一帧按新倍率重新起算
            f_reset(f);
        }
        return true;
    }
    if (strcmp(cmd->cmd, "mode") == 0) {
        int v;
        if (!parse_mode_arg(cmd->arg, &v))
            return false;
        if (p->opts->mode != v) {
            MP_INFO(f, "vfg: mode %d -> %d (vf-command)\n", p->opts->mode, v);
            p->opts->mode = v;
            // Mode 需重新 Load：下一帧 ensure_vfg 检测 vfg_mode 不一致 → 重建
        }
        return true;
    }
    return false;
}

// ── f_reset ────────────────────────────────────────────────────────────────

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;

    // 只清调度状态。**引擎保留**：VFG 每次 Run 显式给两帧（无跨 pair 的
    // 引擎状态需求），重新 Load 代价高（218MB 库的模型初始化），vf_rife
    // 的 f_reset 同款（跨 seek/切换保留引擎）。
    if (p->pending_cur) {
        talloc_free(p->pending_cur);
        p->pending_cur = NULL;
    }
    p->have_prev = false;
    p->prev_pts = 0;
    p->cur_pts = 0;
    p->emit_i = 0;
    p->frame_count = 0;
    p->status_frames = 0;
    p->status_cost_ms = 0;
    p->status_emit = 0;
    p->passthrough = (p->opts->fps == -1);
    if (p->passthrough)
        snprintf(p->pt_reason, sizeof(p->pt_reason), "off");
    // 引擎降级重置（换文件/改参数后允许重试 init）
    p->degrade_warned = false;
    p->engine_failed = false;
    MP_INFO(f, "vfg: reset\n");
}

// ── f_destroy ──────────────────────────────────────────────────────────────

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (p->pending_cur) {
        talloc_free(p->pending_cur);
        p->pending_cur = NULL;
    }

    bool ctx_pushed = false;
    if (p->cuda_ref && p->cuda_ref->ctx) {
        CUresult cr = cuCtxPushCurrent(p->cuda_ref->ctx);
        ctx_pushed = (cr == CUDA_SUCCESS);
    }

    vfg_destroy(&p->vfg);      // 内部等待在途 Run 完成
    p->vfg_configured = false;

    if (p->yuv_conv_ready) {
        yuv_to_rgba_destroy(&p->yuv_conv);
        p->yuv_conv_ready = false;
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
    // cuCtxDestroy（FFmpeg 自建 ctx / hwdec 载体）。
    if (p->av_hw_frames) {
        av_buffer_unref(&p->av_hw_frames);
        p->av_hw_frames = NULL;
    }
    if (p->av_hw_device) {
        av_buffer_unref(&p->av_hw_device);
        p->av_hw_device = NULL;
    }
    if (p->cuda_ref) {
        mp_cuda_ctx_ref_release(p->cuda_ref);
        p->cuda_ref = NULL;
    }
    p->cuda_init_done = false;
}

// ── vf_vfg_filter_info ─────────────────────────────────────────────────────

static const struct mp_filter_info vf_vfg_filter = {
    // ★ .name 同时是 mpv 的日志模块名（filters/filter.c 用 info->name 建
    //   前缀），客户端 --benchmark 设 msg-level=all=no,vfg=status 依赖它。
    .name      = "vfg",
    .priv_size = sizeof(struct priv),
    .process   = f_process,
    .reset     = f_reset,
    .destroy   = f_destroy,
    .command   = vfg_command,
};

// ── vf_vfg_create ──────────────────────────────────────────────────────────

static struct mp_filter *vf_vfg_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_vfg_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    // 启动即确定初值；每帧重估（vf-command 热更新）
    p->passthrough = (p->opts->fps == -1);
    snprintf(p->pt_reason, sizeof(p->pt_reason), "off");
    return f;
}

// ── Registration ───────────────────────────────────────────────────────────

const struct mp_user_filter_entry vf_vfg = {
    .desc = {
        .name          = "vfg",
        .description   = "AI frame interpolation via NVIDIA VFX SDK VideoFrameGeneration",
        .priv_size     = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &vf_vfg_opts_def,
        .options       = vf_opts_fields,
    },
    .create = vf_vfg_create,
};
