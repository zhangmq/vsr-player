# AGENTS.md — vsr-player 项目记忆（DeepSeek Harness 版）

> 本文件由 DeepSeek Harness 转写：来源 = 开发机上的 Claude Code 会话记忆（已筛选：保留知识/经验类 6 个主题，移除过程性/过期记录；清洗隐私/密钥后拉入仓库 `docs/memory/`）+ 原 `CLAUDE.md`（已于 2026-08 删除，信息脱节），并已对照 master 代码（`7bae9ce`）修正其中的失效/冲突信息。
>
> **本文件是项目的唯一权威指令**。原 CLAUDE.md 含未同步的过时内容与幻觉断言历史（modeset、Wayland embedding 两处已被证伪），删除后不再有冲突源；引用任何技术事实前，仍先对照代码验证（通用"证据优先/结论先验证"原则见 `~/.dsh/AGENTS.md`）。

## 项目是什么

Linux 桌面视频播放器：**NVIDIA VFX SDK 实时 AI 超分** + **RIFE 插帧**（TensorRT）。架构 = **libmpv**（demux/decode/AV 同步/timing/VO 全交给 mpv）+ 自定义 mpv 滤镜（`vf_vsr` 超分、`vf_rife` 插帧、`vf_hwup` 软解上传）+ Qt 6/QML 前端（Vulkan，`vo=libmpv` 渲染到 Qt 场景图共享 VkImage）。很可能是首个直接调 VFX SDK（非驱动级 RTX VSR）的开源 Linux 播放器。

滤镜链：`demux → decode → [vf_hwup] → [vf_rife] → [vf_vsr] → VO (libmpv) → Qt scene graph`

## 工作铁律（用户明确要求，违反会被纠正；通用方法论已迁移至 `~/.dsh/AGENTS.md`，此处仅项目特定规则）

1. **严格对齐 mpv 参考实现**——改任何逻辑前先读 `third_party/mpv/` 对应源码，列出差异再改；不自行设计、不加"优化"。mpv 没有的东西不加（除非用户明确要求）。
2. **不用 QTimer 解决渲染/事件架构问题**——业务触发器不解决架构问题；唤醒/驱动走框架生命周期钩子。
3. **字体样式冻结**——不主动改 font family/size/bold，除非用户点名。
4. **日志纪律**——默认只输出 info 及以上（`all=info`）；级别语义对齐 mpv（INFO=状态变更/VERBOSE=低频诊断/DBG=帧级）；调试用 `--msg-level all=v`（帧级 `all=dbg`），benchmark 用 `all=no`。**不要用 `log-file`**（下限 MSGL_DEBUG 会绕过 msg-level 刷屏）。
5. **视觉瑕疵先排除编码**——任何"涂抹/色块"先出 PNG 帧序列/444 编码对照，再归因模型/引擎（yuv420p 色度下采样曾致 3 个引擎全被误判）。

## 技术现状（已对照 master 代码修正）

### RIFE 插帧（重点，状态最新）

- **引擎 = RIFE full FP16 唯一引擎**（`rife_full_fp16.engine`，动态 shape 7ch）——lite 已彻底拆除（FP16 连续推理 ~6 次后 NaN 退化，实证与 TRT 版本无关）；`--rife-model` 参数已删；`vf_rife.c` 无 variant/lite 残留。
- **模型 = RIFE 4.26 v2**（7ch `rife_v2` 变体，`third_party/rife/rife_v4.26_v2_fp16_all.onnx`）——**不是 4.25**（旧记忆仍写 4.25，过时）。切换理由：质量全档胜出（合成 GT PSNR +1~4.6dB，中位移优势最大）。构建：`bash tests/fruc/build_rife_full_engine.sh`（系统 trtexec，动态 profile FP16，`--hardware-compat on` 跨架构 ampere+）。
- **TRT 版本绑定**：引擎内嵌构建时 TRT 精确版本，升级 TRT 后必须重建引擎。当前系统 = **TRT 11.2.1.2**。
- **闪回问题已解决**（`vf_rife.c:622-694`）：整数倍率 ≥2 用 vs-mlrt `Interleave([src, interp])` 语义——源帧总保留、插值帧在相对时间点 t=i/k（tval 恒 i/k，无极端 tval）；**PTS 打在帧的内容时间**（mid 在 `t_prev + tval·(t_cur−t_prev)`、cur 副本在 `t_cur`）——严格单调、无重复无空洞（音频同步下视频间隔=源间隔/k，与音频时钟零漂移）。⚠️ 旧方案（绝对网格 `t0+n/out_fps` + llround 锚定，每对固定 k 帧）在源帧率不是网格整数倍时（如 29.97→60 比例 2.002）会周期性重复/空洞网格点 → mpv `Invalid video timestamp` + 16s 级 desync + 持续丢帧（2026-08-18 test.wmv 播放速度异常，已修）。非整数倍率（<2× 或 24→60=2.5）保留绝对网格（floor 半开区间，无重复但 VFR 源有极端 tval 风险，可接受）。根因背景：绝对网格在 VFR 源（goose 33.0/34.0ms 交替 PTS）上使 tval 摆到 ~0.01–0.99 → 极端 tval 插值 + 源帧被吞 → 内容抖动（此问题由固定 tval 内容解决，与 PTS 方案解耦）。极端 tval（<0.05/>0.95）复制近端点。
- **UI 档位 = [Off, 40, 48, 60]**（`QualityPopup.qml`）——30 已移除、48 已添加；CLI `--fruc` 接受 `off/40/48/60/2/3/4`（2/3/4 为 benchmark 倍率）。**`--fruc 30` 已不接受**。
- 其他机制：场景切换直通（复制时间较近端点）；预取模型（vf_vapoursynth buffered-frames，每 process() 调用输出 1 帧）；EOF flush 最后帧；模式降级时丢弃预取帧防顺序倒挂；`syncVfOptions` 在 FILE_LOADED 后重放 vf 状态（链重建后 UI 修改不丢）。
- **无性能自动降级**（2026-10-15 移除旧的分级降级：曾是"cost 超预算先降档 60→48→40→30，全档超才直通"）。移除理由：判据是**节点内自测**，只能看到 RIFE 自身 GPU 耗时，而真实短缺通常在该节点下游——VSR 按**输出帧**计费（k=out/src 次/对），加上 VO/渲染通路；实测 1080p29.97 + `fps=60` + `scale=auto` 实际上屏 17.5fps、丢帧 +52/s，而判据报 `cost=16.0ms < budget=21.4ms` 全程沉默。目前 `fruc-status` 的 `cost=` 仅作观测保留。⚠️ 移除的代价：4K 纯 RIFE 过载场景失去唯一有效的回退（24fps → 7.3fps）。**完整记录（现象/根因/同族缺陷/vf 节点能力边界/统一方案的待定问题/复现方法）见 `docs/memory/fruc-auto-degrade-removal.md`**；统一的跨节点性能自适应方案（覆盖 rife/vsr/VFX 新版插帧，以实际达成率判定）**待设计，2026-10-15 搁置**。
- 插帧历史决策：NVOFA 硬件光流降级备选（repeat 率太高，有运动对成功率 9.7% vs RIFE 96.3%）；Nano = 两帧平均器已否决（flow 恒零实证）。

### VSR（超分）

- YUV→RGB 用自研 NVRTC CUDA 内核（`yuv_to_rgba.c`，NV12/P010/P016→RGBA U8 或打包 RGB10A2，D2D 全 GPU）——旧 CPU filter graph 方案已被取代（不可再回退）。10-bit 通路见下面的「10-bit（HDR）通路」。
- `vsr_set_output` 已确认：输入不变仅输出尺寸变化**无需重建 VFX**（SetImage 换输出缓冲）；输入变化由 mpv filter 链重建覆盖。
- VFX 对 TRT **零符号调用**（纯冗余，cudnn 仅 GetVersion）——只有 RIFE 需要 TRT 11。`third_party/nvvfx/lib` 无版本 TRT 软链已改名（TRT 10/11 冲突修复），`vsr_proc.c` 用版本化 `.so.10` 加载 TRT、无版本名加载 NPP/cudnn/ngx-vsr。

### 10-bit（HDR）通路（2026-10-17）

**动机**：VFX 1.2 及以前只吃 8-bit，10-bit HDR 源被压成 8-bit 再进 AI（PQ 每个码步 ≈ 3.7–4% 亮度级，暗部单步近 10%，被 2–4× 超分放大成色带）。VFX 1.3 新增 RGB10A2 通路，本次接上；**typemap 全链 10-bit，末端仍由 mpv tonemap 到 8-bit SDR 输出**（HDR 上屏是另一件事，见 `docs/hdr-pipeline-investigation.md`）。

- **开关是自动的**：`vfx_bits_from_imgfmt()`（`yuv_to_rgba.h`）按**单个分量的存储字节数**判定——NV12/RGBA=1 字节→8-bit，P010/P016=2 字节→10-bit，X2BGR10=强制 10-bit。⚠ 不能用 `imgfmt_desc.bpp[0]`（那是整像素位宽：RGBA=32 → 会被误判成 10-bit，实测 SDR 8-bit 走 VFG 时引擎按 RGB10A2 解释 RGBA 字节 → 花屏）。8-bit SDR 内容零回归（仍是 RGBA8 端到端）。
- **NvCV 常量**（反汇编 `NvVFXVideoSuperResImpl::CheckImageEncodingMode` + 探针实测）：`NVCV_RGB10A2=13`、`NVCV_P32=11`；`ImageEncodingMode=1` 必须在 `Load()` 前设，输入/输出格式必须一致。VFG 无开关，按图像格式自动判定（prev/cur/dst 必须同编码）。
- **位序**：RGB10A2 打包字小端 `R bit0-9 / G 10-19 / B 20-29 / A 30-31`（实测 `NvCVImage_Transfer`: RGBA8(255,0,0)→`0xC00003FF`）= **mpv `IMGFMT_X2BGR10`** = `AV_PIX_FMT_X2BGR10` = `VK_FORMAT_A2B10G10R10_UNORM_PACK32`。⚠ **不是** `IMGFMT_X2RGB10`（那是 R 在最高位，R/B 会互换）。
- **FFmpeg CUDA hwframes 不支持 x2rgb10/x2bgr10**（`build/scratch/hwf_test.c` 实测：rgba/bgra/rgb0 ok，x2*10 "Pixel format is not supported"）→ 自建 `hw_frames_ctx` 的 sw_format 恒用 `AV_PIX_FMT_RGBA`（两者都 4B/px、同 stride），10-bit 由**帧参数 `hw_subfmt`** 承载。代价：`hwdownload` 软路径会按 sw_format 解析成 8-bit（本应用 VO 有 CUDA interop，不走该路径）。
- **mpv 的 ra 不认 X2BGR10**（`ra_get_imgfmt_desc` 失败 → CUDA interop 报 `Unsupported format: x2bgr10` → VO 退回 CPU hwdownload + convert，4K 每帧回读）。修法 = mpv overlay 补丁 `src/mpv/video/out/placebo/ra_pl.c`：把 libplacebo 的 `rgb10a2` 格式标成 `special_imgfmt = IMGFMT_X2BGR10` + `special_imgfmt_desc`（components {1,2,3}，同 `ra_gl.c` 对 GL rgb10_a2 的做法）。
- **QualityLevel 语义**（库内错误串 `Valid values: 0-4, 8-19, 21, 23`）：**1–4 = upscale 档，8–11 = 1:1 denoise 档**；混用（如 2× 上采样传 10）→ `Run` 返回 -7 "invalid parameter value"。`vf_vsr` 的 `quality` 选项映射 1–4、`denoise` 映射 8–11，两者不可混。
- **性能**：10-bit 与 8-bit 引擎耗时相同（探针 `build/scratch/vsr10_probe.c`：960x540→1080p 1.36 vs 1.43 ms，1080p→4K 5.39 vs 5.69 ms）；RGB10A2 与 RGBA8 都是 32bpp，拷贝/显存不变。GUI 实测 4K HDR：VSR 1:1 denoise 16.6 ms、VFG@4K 2.9 ms，源 24fps 跟得上。
- **验证证据**：合成 BT.2020/PQ 三色条（420）ours vs mpv 原生路径逐通道吻合（148.0/0/40.8 vs 145/0/39.7 等）；SDR 8-bit 仍是 `cuda[rgba]`、HDR/10-bit SDR 是 `cuda[x2bgr10]`。脚本：`build/scratch/{vsr10_probe.c,hwf_test.c,pixdesc.c}` + `hdr_bars420.mp4`/`hdr1080p_test.mp4`（均为 gitignore 的临时产物）。
- **已知遗留（未修，均有实测证据）**：① `yuv_to_rgba` 的色度按**亮度 span** 归一化（8-bit 用 239 而非 224、P010 用 61439 而非 57344）→ 纯色比 ffmpeg 少 ~6%（红 235 vs 251，10-bit GPU 路径同）；② 4:4:4/4:2:2 hw 帧被 kernel 当 4:2:0 半平面读（合成测试暴露，8-bit 同样存在）；③ `-Dfruc_backend=rife` 分支的 RIFE 冻结在 8-bit（`yuv_to_rgba_init(..., VFX_BITS_8)`），RIFE 链（rife→vsr）因此恒 8-bit——VFG 链才走 10-bit。
- **"软解 10-bit 偏暗"已证伪**（2026-10-17 用户目检 + 数值复核）：同一 app 内软/硬解逐像素一致——`dump-vsr` 的 VSR 输入/输出 PNG 对比 `mean|Δ|=0`、PSNR 138 dB。此前记录的"红 173 vs 235"是**口径错配**（`vo=image` 的原始 PQ dump vs GPU 截图已 tonemap）。注意 CLI 侧的假象：`--vo=gpu --hwdec=no` 时 VO 无 CUDA interop → 退回 hwdownload，会把 10-bit 帧按 hwframes 的 sw_format(RGBA) 解释 → 画面接近全白；app 的自建 VO 有 interop，不出现。
- **运行中切 hwdec（软↔硬）的连环坑（2026-10-17 修）**：
  - 引擎/转换器/流/out_buf 都绑定在**创建它的 CUDA context** 上。旧 `ensure_cuda` 换 context 时只销毁流 → 复用旧 `CUmodule`/engine 句柄 → `cuLaunchKernel` 返回 400（CUDA_ERROR_INVALID_VALUE）→ 滤镜自己报错被 mpv 禁用（"切软解后不插帧、切回也不恢复"的根因）。修法：在旧 context 内 `vsr_destroy`/`vfg_destroy` + `yuv_to_rgba_destroy` + `cuMemFree(out_buf)` + 销毁流，下帧在新 context 重建（`vf_vsr.c`/`vf_vfg.c` 的 ensure_cuda）。
  - `toggleHwaccel()` 原先把位置塞进 `loadfile` 第 4 参数（`start=+5.000`）。mpv 0.41 的第 4 参数是 key/value **列表**（`cmd_loadfile` 里 `cmd->args[3].v.str_list`），`mpv_command` 只能传 char* → 被当 index 解析失败（"The loadfile option must be an integer"）→ **重载从未发生**，而 `set hwdec` 已在当前流中途换解码器（新解码器无参考帧 → HEVC "Could not find ref with POC"）。修法：记 `pendingResumeSeek_` → `loadfile … replace` → `onFileLoadedFromEventThread` 里 seek 回原位置。
  - 残留：属性一改 mpv 立刻换解码器，重载生效前有短暂解码错误抖动。彻底消除需 per-entry hwdec（`mpv_command_node` 传 `["loadfile",url,"replace",-1,["hwdec",…,"start",…]]`）或 stop→set→reload（会动播放列表，未采用）。


### HDR 上屏（spike 结论，2026-10-17）

调研全文见 `docs/hdr-pipeline-investigation.md` §10。要点（**都与具体显示器无关**，
本机 400nit 面板只用于验证机制）：

- **前提：Qt 的 HDR swapchain 只在 Vulkan RHI 后端生效**。Wayland 下 Qt 默认
  OpenGL(EGL) → `_qt_sg_hdr_format` 被静默忽略、`isFormatSupported(HDR10/scRGB/P3)` 全 0
  （dmabuf fourcc 恒 AR24）——踩过这个坑并据此得出过错误结论。本项目 app 走 Vulkan ✔。
- 入口是**动态窗口属性 `_qt_sg_hdr_format`**（"hdr10" / "scrgb" / "p3"，或 env
  `QSG_RHI_HDR`），由 `QSGRhiSupport::applySwapChainFormat()` 在 swapchain 创建时读取；
  Vulkan 后端实测 `isFormatSupported` 三项全 1、`QRhiSwapChain::format()==HDR10`。
- **色彩空间声明走公开 API**：`QSurfaceFormat::setColorSpace(Bt2020+ST2084)` → Qt 发
  `wp_color_management`（bt2020=6/st2084=11）→ KWin 回 `ready`；三种声明观感不同，
  PQ 声明下 Qt 把 UI 按 PQ 编码、往返不偏色（sRGB 105.4 / PQ 102.8 / Linear 138.1）。
- ⚠ **不要用 Qt 当显示能力的来源**：Linux 上 `QRhiSwapChain::hdrInfo()` 返回 Qt 硬编码
  默认值（min 0 / max 1000 nit / sdrWhiteLevel 200），只有 Windows 有真实实现。峰值/色域必须
  从合成器读（`wp_color_management` 的 output/surface feedback——mpv wayland 后端即此法），
  需要我们自建协议客户端（已有 `WaylandIdleInhibit.cpp` 先例）或配置覆盖。
- 格式选择要有 fallback 且逐屏判定：HDR10(`A2B10G10R10+HDR10_ST2084`) → scRGB
  (`RGBA16F+EXTENDED_SRGB_LINEAR`) → SDR；`isFormatSupported()` 是 per-surface，
  窗口换屏必须重新评估。自建 Vulkan instance 需加 `VK_EXT_swapchain_colorspace`。
- 运行期开关：色彩空间声明可运行期改；**swapchain 格式**切换（SDR↔HDR10）需重建
  swapchain + Qt 的 swapchain render pass + 我们的 composite render pass
  （`applySwapChainFormat` 只在创建时调一次，要自己驱动或重建窗口）。

### 渲染循环（Qt Quick）

- **纯请求驱动**：mpv update callback（按帧率推送；无活动时完全静默）→ QueuedConnection → 条件 requestRender（`mpv_->update() > 0 || renderRequested_`）→ updatePaintNode → render + reportSwap + vkQueueWaitIdle。**无心跳/无自主循环**——旧文档的"200ms 心跳"模型已证伪。
- `QSG_RENDER_LOOP=basic` 必须（threaded 每帧渲染饿死事件循环）。**绝不无条件 update()/postEvent**（渲染风暴回归教训）。`aboutToBlock` 兜底已删除（`Video.cpp:377` 注释确认）。
- 卡死归因：目录播放概率性卡死根因 = filter 每帧 `get_render_target_size` 持锁等渲染线程 → vo_libmpv.c 无锁读（__atomic relaxed）已修复。
- **Xid 109**（全屏切换 device loss）：根因 = `Video::ensureRenderTarget` 销毁 rtImage 前无 GPU 同步（Vulkan UAF）→ 销毁前无条件 `vkQueueWaitIdle` 修复（`tests/fs_stress.sh` 验证）。

### 启动与会话行为（2026-10-17）

- **无参数启动 = idle**：只恢复上次播放列表（`restorePlaylistIdle()` 全 `loadfile … append`，mpv 无 current 条目 → 不解码、不建滤镜链、不 Load 引擎、不做续播 seek），按播放才 `resumeLastPath()` → `playlist-play-index` 载入，mpv 用 watch_later 自动恢复上次进度。命令行给了文件、或显式 `-- --pause=yes` → 立即载入（mpv 语义）。
- 起因：曾有"启动暂停"实现（`pause=yes` 选项 + 启动即载入），但 mpv 精确 seek 会把"关键帧→目标位置"的帧**过完滤镜链再丢掉**（实测 `start=12.302`/关键帧 6.25s → 142 源帧、VFG 570 输出帧白跑、海报晚 1.6s）；日志里那批 `vfg-status` 是丢弃帧不是播放。完整测量与被否方案：`docs/memory/startup-idle-policy.md`。
- idle 时 mpv 的 `pause` 初值为 false → `pause` 观察器按 `updatePlaying(!p && fileLoaded_)` 判定，否则 UI 未载入就显示"播放中"。

### 桌面集成（2026-10-17）

- `scripts/install.sh` 装应用菜单入口：入口内容 = **`data/vsr-player.desktop.in`**（单一事实源，中英 Name/Comment；`@EXEC@` 由脚本替换为安装后绝对路径，不依赖 PATH），图标 = `data/vsr-player.svg` + `.png` → hicolor `scalable`/`512x512`，之后 best-effort 刷新 `update-desktop-database`/`gtk-update-icon-cache`/`kbuildsycoca6`。打包脚本把 `data/` 放进 tarball 根（`build_release.sh` staging）。
- `main.cpp` 里 `QGuiApplication::setDesktopFileName("vsr-player")`：Wayland app_id / X11 WM_CLASS 钉成与 `.desktop` 同名，否则任务栏图标与窗口分组会失配。
- ⚠️ **图标是占位**（用户 2026-10-17："后续做个像样的"）：正式图标只需替换 `data/vsr-player.svg`（矢量单一事实源）并跑 `scripts/render-icon.sh` 重生成 PNG。
- `Exec=… %f`（单文件/目录）**不是** `%U`：`Options::parse` 只接受一个位置参数，多文件会 `error: multiple video files`。

### 构建

- **patch 方案**：`third_party/mpv`（纯净 0.41，只读基座）+ `src/mpv` 覆盖层（只含修改文件）。改 mpv 代码 = 改 `src/mpv/<同路径>` 后跑 `scripts/build_mpv.sh`（合并到 `build/mpv` 再 meson 构建）。
- **构建陷阱**：`build_mpv.sh` 输出被 grep 过滤，编译失败看不到——确认最后一行 "Done"；别 `cd build` 后还用相对路径；系统库升级（FFmpeg/Qt/TRT）后必须全量重建（soname 变更直接启动失败）。
- 分发（2026-09 重构）：**源码构建为推荐路径**——`scripts/build-from-source.sh`（依赖自检→构建→安装→记录快照）；系统库升级后重跑同一条命令即可恢复。预编译 tarball 两个变体：`build_release.sh`（standard，需系统 Qt ≥6.12——Qt 二进制兼容是单向的，构建机 Qt 6.12.0 → 真实下限 6.12）与 `--variant=full`（捆绑最小 Qt：镜像发行版布局 `<prefix>/lib/qt6/{plugins,qml}` + Qt 库在 `<prefix>/lib/`，靠 Qt 自带相对 RUNPATH 解析，**免 patchelf**；`main.cpp` 在 QGuiApplication 前探测并设 `QT_PLUGIN_PATH`/`QML2_IMPORT_PATH`）。依赖收集 = 显式核心集 + 媒体栈动态发现（libcdio/libdvd/libbluray）+ **打包时闭包自检**（未捆绑且非白名单即失败）；`--engine=<已构建引擎>` 跳过 trtexec。
- **依赖漂移检测**：`scripts/check-deps.sh [--record]` 记录构建时**系统库**快照（路径+mtime+size，排除仓库内自建产物——否则每次重建都误报），系统升级替换后提示重建；staging 目标用 `LD_LIBRARY_PATH` 模拟安装态解析。**soname 变更导致启动失败 → 跑 `build-from-source.sh` 重建**；用户侧排查见 README「故障排查」。
- `scripts/install.sh`（用户端）：~/.local；VFX 从 **NVIDIA 官方索引 `pypi.nvidia.com`** 取 nvidia-vfx wheel（pypi.org 仅占位 sdist，2026-09 实测），curl 提取不调 pip 不改系统；VFX ~0.6GB 不随 tarball 分发（SLA）。⚠️ `set -euo pipefail` 下所有探测命令（nvidia-smi / pkg-config / curl 管道）必须 `|| true` 兜底，否则"未检测到"会变成"安装静默中止"（本项目实测踩过两次）。

## 环境速查

- GPU：RTX 5060 Ti（Blackwell sm_120）；驱动 615.78.08（2026-10-17 实测）；KWin Wayland 6.8.0（纯 Wayland，无 X11）。显示器 DP-3 = LG ULTRAFINE 27" 4K：EDID 含 ST2084、峰值 400 nit、基色≈DCI-P3（HDR 可在 KWin 里开）
- Qt 6.12.0 / C++20 (GCC 13+) / Meson / FFmpeg 9.0 / TRT 11.2.1.2（pacman）
- 测试文件：`input/`（catlove_720p.webm、Jellyfish 1080p h264/h265、goose_480p、heya 等）
- 复现命令：`./build/src/client/vsr-player --fruc 60 input/goose_480p.webm`；RPC socket `/tmp/vsr-player.sock`
- benchmark：`./build/src/client/vsr-player --benchmark [--scale off|auto|2|3|4] <video>`（无 UI，`all=no`，END_FILE 判定结束）；短文件启动开销会低估吞吐 ~35%，用 ≥100s 文件取稳态
- 解码规则：AV1/H.264/HEVC/VC-1（wmv3/vc1）/MPEG-1/MPEG-2/MJPEG 一律 native decoder + CUDA hwaccel（get_format，mpv 显示为 `nvdec`）；**永不用 `*_cuvid`**（surface 管理 bug → 周期重复帧）。⚠️ 白名单补全史（2026-08-18，patch `src/mpv/video/decode/vd_lavc.c`）：默认 hwdec-codecs 漏 wmv3（软解 core 99.7% CPU + 掉帧，加后硬解 CPU 10% 0 掉帧）；实测 mpeg2video/mpeg1video/mjpeg 硬解生效已加入；mpeg4/h263/h263p 未加（mpeg4 的 mpv nvdec 路径 probe 失败打 error `decoding to AV_PIX_FMT_NONE` 回退软解，ffmpeg CLI 却可解——集成差异；h263 无测试文件）。

## 调试工具/关键路径

- OSD FRUC 状态行 + stderr `[mpv status]`（fruc-status 每 30 帧，`MSGL_STATUS` 通道，mpv 补丁 `src/mpv/common/msg.c`）
- RPC 热切换：`{"command":["command","vf-command","rife","fps","X"]}`、seek（socket `/tmp/vsr-player.sock`）
- VSR 帧对截图（正式调试命令）：`vf-command @vsr dump-both <in.png>|<out.png>`——同一处理帧的 VSR 输入/输出双侧 PNG（mpv 自带 screenshot 只截 filter 后帧，不能截 VSR 输入）
- 渲染节拍：`VSR_LOG_VIDEO=1` + stderr 时间戳；帧序列提取：untimed benchmark + each-frame 截图（暂停+截图不可靠，retained 旧帧）
- 关键文件：`src/mpv/video/filter/`（vf_vsr.c/vf_rife.c/vf_hwup.c/vsr_proc.c/rife_proc.c/rife_trt.cpp/cuda_shared.h/yuv_to_rgba.*）；`src/client/`（main.cpp/MpvController/PlayerViewModel/Video.cpp/Options.cpp/RpcServer.cpp）
- 视觉问题数值化：`tests/verify_frames.sh`（baseline/test 两阶段 PSNR）——无 baseline 不改代码
- **gdb 调试**（C/C++ 崩溃/卡死）：`gdb -batch -ex run -ex bt <binary>`（崩溃看调用栈；卡死看线程）——SIGSEGV→bt 定位非法访问；SIGABRT→bt+寄存器看 assert 条件；卡死→bt 看所有线程在等什么。通用排查流程见 `~/.dsh/AGENTS.md` 调试方法论

## 相关文档

- `README.md`/`README_zh.md`（功能/构建/分发/CLI）、`docs/`（设计记录与参考索引）。调试守则/方法论在 `~/.dsh/AGENTS.md`（全局）
- **`docs/memory/`** — 历史调研/调试记录（源自 Claude Code 会话记忆，筛选保留 6 个知识类主题 + `README.md` 索引；过程性/过期记录已移除）。⚠️ 部分内容已过时（如 RIFE 4.25、闪回未解决），引用前对照代码，以本文件为准
