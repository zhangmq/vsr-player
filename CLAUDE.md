# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Prime Directive

**代码由 AI agent 编写。永远选择最佳方案，不考虑实现成本。**

- 架构级修改欢迎——只要能产生更正确、更可维护的代码。
- 不做"改最小"的妥协——如果正确方案需要重构基础设施，就重构。

## Auto-Mode Workaround

模型权限判断偶尔误杀正常的编译/测试命令。遇到 "Auto mode could not evaluate" 拒绝时，直接重试 1-2 次即可——通常第二次就会通过，无需修改命令内容。

## No Sudo

**永远不要调用 `sudo`。** 任何需要特权的操作（安装系统包、修改系统配置等），标记为需用户执行并告知用户具体命令。

## Debugging Discipline

### Step Zero: Classify the Problem

| AI can verify? | Examples |
|---|---|
| ✅ Yes | compile/link errors, crash with backtrace, test failure, deterministic wrong output |
| ❌ No | screen corruption, color errors, audio artifacts, stutter, GPU behavior |

### For Problems AI Cannot Verify (visual/audio)

Use `tests/verify_frames.sh` to convert a perception problem into a numeric one:

1. **Baseline** (human once): `./tests/verify_frames.sh baseline <video> <frames>` — FFmpeg CLI extracts Y-plane PNGs. Human confirms they look correct.
2. **Iterate** (AI loop): `./tests/verify_frames.sh test <video> <frames>` — builds player, dumps same frames, runs FFmpeg PSNR comparison. Outputs PASS/FAIL table. AI reads numbers — no human needed.
3. **Stop condition**: all frames PASS → ask user to review → commit.

**Rules:**
- No baseline → no code change.
- User says "no change" → revert immediately, don't stack guesses.
- Log errors are clues, not root cause.
- Build + let user test before committing.

### For Problems AI Can Verify

Systematic debugging: reproduce → gather evidence → single hypothesis → minimal test → verify → commit.

### Test Execution Constraint

When running automated tests:
- **Only collect and report results.** Do NOT automatically diagnose failures or modify code.
- If any test fails, STOP and present the results to the human. Do not proceed to fix without human review.

### Logging Discipline

**Log levels reflect message importance, never chosen to satisfy output requirements.**

**默认只输出 info 及以上**（warn/err/fatal 默认可见）。级别语义（mpv 官方，勿自创）：

| Level | When to use |
|-------|-------------|
| `MP_FATAL` / `MP_ERR` | Fatal errors, assertions that should never happen |
| `MP_WARN` | Recoverable problems, degraded operation |
| `MP_INFO` | 状态变更节点（加载/切换/模式决策，如 scale computed、warmup complete、file loaded） |
| `MP_VERBOSE` | Intermediate diagnostic info（如 VO reconfig params、RT dimensions recorded）——低频，非每帧 |
| `MP_DBG` | Per-frame or per-event diagnostics（如 frame output params、stride verification）——**帧级别在这里，不是 VERBOSE** |

**Never** raise a log level just to make messages visible — adjust the client's `msg-level` instead. 默认 `all=info`；调试细粒度日志用 `--msg-level all=v`（帧级 `all=dbg`）。benchmark 模式 `all=no`。

**mpv 日志通道**：libmpv 经 `mpv_request_log_messages("info")` + 事件线程 `MPV_EVENT_LOG_MESSAGE` 转发 stderr（benchmark 请求 `"no"` 全静默）。**不要用 `log-file`**——其过滤级别下限为 `MSGL_DEBUG`，会绕过 `--msg-level` 把 verbose/debug 全灌出来（曾致默认输出刷屏）。

**client 层**（`src/client/Log.h`，独立轻量体系，mpv 日志单向不可注入）：级别与 mpv 对齐，`MLOG_INFO`/`MLOG_WARN`/`MLOG_ERR`；benchmark 全静默（`vsr_log_set_quiet(true)`，与 `all=no` 对齐——"benchmark 无日志"是测量口径）。CLI 解析错误（Options.cpp）直接 stderr，不经过日志体系（入口交互反馈）。渲染循环逐帧诊断（Video.cpp VLOG）默认关闭，`VSR_LOG_VIDEO=1` 门控。

## Project Overview

A Linux desktop video player that applies real-time AI super-resolution to video playback using the NVIDIA Video Effects SDK. Based on **libmpv** — all demux, decode, A/V sync, timing, and rendering are handled by mpv. VSR is injected as a custom mpv video filter (`vf_vsr`).

**This is likely the first open-source Linux player using the VFX SDK directly (not driver-level RTX VSR).**

## Architecture

```
┌────────────────────────────────────────────────┐
│  Qt6+QML Frontend (main thread)                │
│                                                │
│  MpvController — libmpv API wrapper            │
│  ├── mpv_create / set_option / initialize      │
│  ├── vo=libmpv（自定义 VO：mpv 渲染到 Qt 侧    │
│  │   VkImage，Qt 场景图合成呈现）              │
│  └── mpv_render_context（Vulkan，共享 device） │
│                                                │
│  QML UI — TopBar, BottomBar, Playlist, OSD     │
├────────────────────────────────────────────────┤
│  mpv internal pipeline                         │
│                                                │
│  demux → decode → [vf_hwup] → [vf_rife] → [vf_vsr] → VO (libmpv)      │
│                ↑            ↑              ↑                          │
│           SW→CUDA upload   RIFE (TRT)    VFX SDK + CUDA               │
│                                                                       │
│  mpv manages: A/V sync, timing, seek, VO                              │
└───────────────────────────────────────────────────────────────────────┘
```

- vf_hwup: SW 帧（软解）上传 CUDA，下游滤镜只处理硬件帧
- vf_rife: RIFE 插帧（TensorRT），超分之前以源分辨率运行（fps/scale/adaptive 选项）
- vf_vsr: custom mpv video filter, receives `mp_image`, upscales via CUDA+VFX SDK, outputs upscaled `mp_image`
- Frontend reuses existing QML UI components where possible
- VFX SDK bundles ~1.1GB of .so files；**不随发布 tarball 分发**（SLA 限制再分发，install.sh 引导从 PyPI 官方 nvidia-vfx wheel 获取）

## Repository Structure

**mpv 采用 patch 方案**：`third_party/mpv` 是完整 mpv 0.41 源码（参考 + 构建基座，不修改）；`src/mpv` 是覆盖层（镜像 mpv 树结构，只含修改过的文件）；`scripts/build_mpv.sh` 合并两者构建。**修改 mpv 代码 = 改 `src/mpv` 对应文件后跑 `scripts/build_mpv.sh`。**

```
src/
├── mpv/                  # mpv patch 覆盖层（镜像 mpv 树，只含修改文件）
│   ├── video/filter/     #    vf_vsr.c、vsr_proc.c、yuv_to_rgba.c、vf_hwup.c、
│   │                     #    vf_rife.c、rife_proc.c、rife_trt.cpp（新增）
│   ├── video/out/        #    vo_libmpv/wayland/x11/vulkan 补丁 + libmpv_vk.c
│   ├── filters/ options/ #    上游文件补丁（user_filters、f_output_chain…）
│   └── meson.build       #    上游 meson.build 补丁（vf_vsr 注册等）
├── client/               # Qt6+QML frontend
│   ├── main.cpp          #    Entry point
│   ├── MpvController.h/.cpp   libmpv wrapper
│   ├── RpcServer.h/.cpp  #    JSON IPC（Unix socket）
│   ├── PlayerViewModel.h/.cpp UI 状态单一事实源（属性观察器 + osdTextString）
│   ├── PlaylistModel.h/.cpp 播放列表增量镜像（QAbstractListModel）
│   ├── Log.h/.cpp        #    client 分级日志（MLOG_INFO/WARN/ERR）
│   ├── shaders/               GLSL → SPIR-V shaders
│   ├── translations/          .ts/.qm（英文 source，zh_CN 提供翻译）
│   └── ui/                    QML overlay/controls（BottomBar 含进度条+热区一体）

third_party/
├── mpv/                  # mpv 0.41 完整源码（参考 + 构建基座，只读）
├── nvvfx/                # VFX SDK headers（开发者自行准备，不分发）
├── cuda12/               # CUDA 12.9.2 存档（redist tar.xz，自行下载、gitignored；官方 OF SDK 需 12 的 cuCtxCreate 3 参签名）
├── material-icons/       # 图标字体
└── rife/ openframegen/   # RIFE 模型 / NVOFA 调研素材

references/               # 外部项目源码参考（只读，非构建输入）
├── celluloid/ glfw/ mpvqt/ vlc/ qtbase/ qtdeclarative/ qt6-multimedia-quick/

docs/
├── mpv-reference-index.md     # mpv source → vsr-player cross-reference
└── images/                    # Screenshots, diagrams

scripts/
├── build_mpv.sh              # 合并构建：cp third_party/mpv → build/mpv，覆盖 src/mpv，meson 构建
│                             #   （MPV_BUILD_DIR/DIST_RPATH/BUILDTYPE 环境变量可覆盖——dist 构建用）
├── build_release.sh          # 发布构建：mpv/client dist（release + $ORIGIN RPATH）→ 依赖库收集
│                             #   → ampere+ engine → tarball（build/vsr-player-<ver>-*.tar.xz）
├── check-deps.sh             # 依赖检查（third_party + 构建工具）
├── install.sh                # 用户端安装（tarball/dev 双场景自适应）：二进制 + 捆绑库 + engine
│                             #   + 字体 + 翻译 → ~/.local；VFX 缺失时 curl 从 PyPI 官方
│                             #   nvidia-vfx wheel 纯下载提取（环境纯净：不调 pip 不 sudo 不改配置）
├── install_mpv_local.sh      # 独立 mpv-vsr CLI 安装到 ~/.local（开发者本地用；wrapper 配套）
└── mpv-vsr-wrapper.py        # CLI 包装脚本（src/scripts/，开发者本地 Chrome 插件配套，不进分发）

tests/
├── verify_frames.sh          # 视觉问题数值化验证（baseline/test 两阶段 PSNR）
└── fruc/                     # FRUC 实验与引擎构建（build_rife_full_engine.sh 用系统 trtexec 构建
                              #   动态 shape ampere+ 引擎；convert_rife_onnx_fp16.py 4.26 ONNX 转换；
                              #   run_rife.py ORT 真值对比；official/ OF SDK 调研源码只读）
```

## Key Findings

### YUV→RGB color conversion

FFmpeg's `scale_cuda` filter **cannot** convert semiplanar YUV (NV12/P010) to packed RGB on this system — the CUDA kernel for these conversions is not compiled (`CUDA_ERROR_NOT_FOUND`). Workaround: single CPU filter graph `buffersrc → scale → format=rgba → hwupload_cuda → buffersink`. HW frames are downloaded via `av_hwframe_transfer_data` before feeding. SW frames feed directly. Single graph handles both paths.

The FFmpeg `scale` filter (CPU sws_scale) correctly reads frame metadata (colorspace, color_range, color_trc) and produces pixel-perfect RGB output — verified 46.5 dB PSNR against FFmpeg CLI baseline.

### Codec selection rules

| Codec | HW path | SW fallback | Never use |
|-------|---------|-------------|-----------|
| AV1 | `av1` + `av1_nvdec` hwaccel | `libdav1d` | `av1_cuvid` |
| H.264 | `h264` + `h264_nvdec` hwaccel | software | `h264_cuvid` |
| HEVC | `hevc` + `hevc_nvdec` hwaccel | software | `hevc_cuvid` |

**Golden rule:** Native decoders + get_format hwaccel. Never `_cuvid` variants (surface management bug → periodic duplicates). Verified: 7202 frames, 0 duplicates.

```c
codec = avcodec_find_decoder_by_name("av1");  // NOT libdav1d, NOT av1_cuvid
avcodec_parameters_to_context(codec_ctx, codecpar);  // extradata required!
codec_ctx->get_format = get_hw_format;          // → AV_PIX_FMT_CUDA
codec_ctx->hw_device_ctx = av_buffer_ref(hw);
avcodec_open2(codec_ctx, codec, nullptr);
// hwaccel (av1_nvdec) activates after first frame — codec_ctx->hwaccel
// is NULL until then.
```

### NVIDIA Vulkan + Wayland

NVIDIA driver supports `VK_KHR_wayland_surface` **if** `nvidia_drm.modeset=1` (kernel cmdline). Check:

```bash
sudo cat /sys/module/nvidia_drm/parameters/modeset  # must be Y
```

Without modesetting, `vkGetPhysicalDeviceSurfaceSupportKHR` returns `VK_FALSE`.

### Wayland embedding

mpv `--wid` only works on X11. For native Wayland embedding, we patch mpv's
Wayland VO to accept an external `wl_surface*` from Qt, creating a subsurface
instead of a toplevel `xdg_surface`. mpv renders its video/OSD surfaces as
subsurfaces of Qt's main surface — single atomic commit, no tearing.

### VSRProcessor — Full Dependency Chain

```
libnvVFXVideoSuperRes.so (62K)     ← VSR C API, our direct dependency
  ├── libVideoFX.so (40K)           ← VFX core framework
  ├── libVideoFXLocal.so (5.8M)     ← local processing pipeline
  ├── libNVCVImage.so (4.4M)        ← CUDA image utilities
  ├── libnvngxruntime.so (79K)      ← NGX runtime loader
        └── libnvidia-ngx-vsr.so (44M)   ← NGX VSR inference engine
              ├── libnvinfer.so.10 (641M)       ← TensorRT
              ├── libnvinfer_plugin.so.10 (53M) ← TensorRT plugins
              ├── libnvonnxparser.so.10 (4.3M)  ← ONNX model parser
              ├── libcudnn.so.9 (123K)          ← cuDNN
              └── libnpp*.so.12 ×9 (292M)       ← CUDA NPP
  └── libcuda.so.1                   ← NVIDIA driver (system, only external dep)

Total bundled: ~1.1GB.
```

### Other findings

- VFX SDK bundles all deps (TensorRT, NPP, cuDNN). Only `libcuda.so.1` is external.
- VSR internal CUDA streams require device-level sync, not stream-level.
- `hwaccel` field on AVCodecContext is NULL after `avcodec_open2` — set after first frame.
- `avcodec_parameters_to_context()` is REQUIRED for hwaccel init (provides extradata).
- `Item` has no `font` property in any Qt 6 version — use empty string for system default font.
- Qt 6.11 signal handlers with parameters must use explicit `function(param) {}` syntax.

## Render Loop Scheduling Model

This is the **authoritative scheduling model** for the Qt Quick frontend (Video/CompositeRenderNode). Do not deviate.

### Core insight

mpv's `mpv_render_context_set_update_callback` is a **per-frame notification**: while playing, it fires **stably at the video frame rate** (frequency = vo draw_frame frequency); when there is no activity (paused, ended, seek-settled), it is **completely silent** — it is NOT a periodic heartbeat.

### Model (request-driven, current implementation)

```
// ── Update callback（核心线程）────────────────────────────
// 播放中每帧推送（帧率）；无活动完全静默。只负责通知 + 唤醒。
mpv_render_context_set_update_callback(ctx, () => {
    renderRequested_.store(true, release)
    QMetaObject::invokeMethod(video, "requestRender", QueuedConnection)
})

// ── GUI 线程：requestRender（条件投递）───────────────────
fn requestRender():
    if !mpv_->update() > 0 && !renderRequested_.load():   // VO 无 pending
        return                                            // 零渲染零空转
    update()                              // 标记 dirty → sync 时 updatePaintNode
    postEvent(window, UpdateRequest)      // Wayland QPA requestUpdate 丢弃兜底

// ── 场景图同步阶段：updatePaintNode（每次渲染请求一次）──
fn updatePaintNode():
    uf = mpv_->update()
    if uf > 0 || renderRequested_.exchange(false):
        mpv_render_context_render(ctx, params)   // 消费 VO 帧队列
        mpv_render_context_report_swap(ctx)
        vkQueueWaitIdle(queue_)                  // 排空 GPU（rtImage 引用安全）
```

**纯请求驱动**：每次 callback → 渲染一次。无自主循环、无 isAlive、无停滞检测。渲染频率 = callback 频率 = 帧率。

### Qt Quick implementation（现行，Video/CompositeRenderNode）

渲染循环由 QQuickView basic 渲染循环承载（`QSG_RENDER_LOOP=basic`，main.cpp 设置——threaded 模式与 untimed 渲染不兼容，曾致双 vblank 30fps；threaded 下每帧渲染使 GUI 事件饿死，EVT 400-1350ms）。mpv 渲染走 `mpv_render_context_render` 到共享 VkImage，`report_swap` 告知显示时刻，FIFO present 提供隐式帧节拍。

### Key properties

- **No hardcoded timeouts.** Frame pacing comes from the present/swap mechanism or compositor callback, never from `sleep(n)` or `WaitEventsTimeout(n)`.
- **No busy-polling when idle.** No rendering request (VO pending) → no render scheduled — the scene graph goes idle, waiting for the next callback.
- **Never unconditionally `update()`/`postEvent`.** Unconditional posting creates a render-event storm that starves the event loop (EVT 400-1350ms, measured). 15a6f15's `aboutToBlock` fallback violated this and regressed UI latency — conditional requestRender only.
- **Retained frame semantics.** mpv's render target (VkImage / FBO) inherently retains the last rendered frame. When no new frame arrives, the last frame stays on screen without any extra work.

## Performance Baseline

**不得自行假设存在性能瓶颈。** 只有当用户明确告知（如"当前测试是性能受限场景"、"需要跳帧"等），才考虑性能不足以实时播放的情况。默认认为硬件性能足够。

### FFmpeg CLI 裸解码基准 (ffmpeg -f null -)

纯 decode，无 filter，无 render。仅作解码器能力上限参考。

```bash
ffmpeg -threads auto -i <file> -an -f null -           # SW decode
ffmpeg -hwaccel cuda -hwaccel_output_format cuda \
  -threads 1 -i <file> -an -f null -                     # HW decode (nvdec)
```

| 测试文件 | 编码 | 分辨率 | 源fps | SW fps | HW(nvdec) fps |
|---------|------|--------|-------|--------|---------------|
| input/catlove_720p.webm | AV1 | 1280×720 | 60 | 2246 | 3856 |
| Fallout S02E02 (HEVC 4K) | HEVC | 3840×1600 | 24 | 197 | 651 |

### vsr-player benchmark 模式（decode + vf_vsr + vo=libmpv + Qt Quick 渲染循环）

```bash
./build/src/client/vsr-player --benchmark [--scale off|auto|2|3|4] <video>
```

- benchmark 无 QML UI；OSD 用 mpv 内部 osd-msg1（格式串每帧求值 +
  独立线程 1Hz 更新渲染帧率）；END_FILE 事件判定结束 → 命令行 summary
- **测量口径**：用加长版（≥100s）文件取稳态——短文件（10s）的启动
  开销（加载/VSR 初始化/warmup）会使 passthrough 吞吐低估 ~35%
- throughput = 渲染帧数 / 自首帧起 elapsed（与 OSD render fps 同口径）
- `estimated-vf-fps` 属性基于帧 PTS 间隔 = 视频帧率，非渲染帧率，
  benchmark 中无意义

| 测试文件 | 编码 | 分辨率 | passthrough fps | VSR 2× fps |
|---------|------|--------|----------------|------------|
| input/catlove_720p.webm (120s) | AV1 | 1280×720 | 1670 | 270 |
| input/Jellyfish_1080_100s_h264.mp4 | H.264 | 1920×1080 | 1303 | 120 |
| input/Jellyfish_1080_100s_h265.mp4 | H.265 | 1920×1080 | 1307 | 121 |

（VSR 2× 输出 2160p；1080p VSR 计算量 4 倍于 720p，故 fps 更低。）

## Environment

- **Qt:** 6.11.1 (CachyOS, pacman)
- **C++:** C++20 (GCC 13+)
- **Build:** Meson (mpv 原生构建系统 + Qt 前端)
- **QML:** Qt Quick Controls (plain `import QtQuick.Controls` — project convention, not `.Basic`)
- **mpv:** patch 方案——third_party/mpv 完整源码 + src/mpv 覆盖层，`scripts/build_mpv.sh` 合并编译（构建树 build/mpv）

## Reference Source Code

| Directory | Purpose |
|-----------|---------|
| `third_party/mpv/` | mpv 0.41 — filter API, VO, playloop（参考 + patch 基座） |
| `references/celluloid/` | Celluloid (GNOME mpv frontend) — libmpv integration reference |
| `references/` | 其他外部项目源码（glfw、mpvqt、vlc、qtbase、qtdeclarative） |

## External References

- [NVIDIA Video Effects SDK](https://developer.nvidia.com/video-effects-sdk)
- [NVIDIA Maxine Linux VFX SDK (EA) on NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea)
- [FFmpeg hw_decode.c — CUDA decode example](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c)
