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
- 其他机制：分级降级（60→48→40→30 档位超预算自动降档，4K 不扣 VSR 预算）；场景切换直通（复制时间较近端点）；预取模型（vf_vapoursynth buffered-frames，每 process() 调用输出 1 帧）；EOF flush 最后帧；模式降级时丢弃预取帧防顺序倒挂；`syncVfOptions` 在 FILE_LOADED 后重放 vf 状态（链重建后 UI 修改不丢）。
- 插帧历史决策：NVOFA 硬件光流降级备选（repeat 率太高，有运动对成功率 9.7% vs RIFE 96.3%）；Nano = 两帧平均器已否决（flow 恒零实证）。

### VSR（超分）

- YUV→RGB 用自研 NVRTC CUDA 内核（`yuv_to_rgba.c`，NV12/P010/P016→RGBA U8，D2D 全 GPU）——旧 CPU filter graph 方案已被取代（不可再回退）。
- `vsr_set_output` 已确认：输入不变仅输出尺寸变化**无需重建 VFX**（SetImage 换输出缓冲）；输入变化由 mpv filter 链重建覆盖。
- VFX 对 TRT **零符号调用**（纯冗余，cudnn 仅 GetVersion）——只有 RIFE 需要 TRT 11。`third_party/nvvfx/lib` 无版本 TRT 软链已改名（TRT 10/11 冲突修复），`vsr_proc.c` 用版本化 `.so.10` 加载 TRT、无版本名加载 NPP/cudnn/ngx-vsr。

### 渲染循环（Qt Quick）

- **纯请求驱动**：mpv update callback（按帧率推送；无活动时完全静默）→ QueuedConnection → 条件 requestRender（`mpv_->update() > 0 || renderRequested_`）→ updatePaintNode → render + reportSwap + vkQueueWaitIdle。**无心跳/无自主循环**——旧文档的"200ms 心跳"模型已证伪。
- `QSG_RENDER_LOOP=basic` 必须（threaded 每帧渲染饿死事件循环）。**绝不无条件 update()/postEvent**（渲染风暴回归教训）。`aboutToBlock` 兜底已删除（`Video.cpp:377` 注释确认）。
- 卡死归因：目录播放概率性卡死根因 = filter 每帧 `get_render_target_size` 持锁等渲染线程 → vo_libmpv.c 无锁读（__atomic relaxed）已修复。
- **Xid 109**（全屏切换 device loss）：根因 = `Video::ensureRenderTarget` 销毁 rtImage 前无 GPU 同步（Vulkan UAF）→ 销毁前无条件 `vkQueueWaitIdle` 修复（`tests/fs_stress.sh` 验证）。

### 构建

- **patch 方案**：`third_party/mpv`（纯净 0.41，只读基座）+ `src/mpv` 覆盖层（只含修改文件）。改 mpv 代码 = 改 `src/mpv/<同路径>` 后跑 `scripts/build_mpv.sh`（合并到 `build/mpv` 再 meson 构建）。
- **构建陷阱**：`build_mpv.sh` 输出被 grep 过滤，编译失败看不到——确认最后一行 "Done"；别 `cd build` 后还用相对路径；系统库升级（FFmpeg/Qt/TRT）后必须全量重建（soname 变更直接启动失败）。
- 分发：`scripts/build_release.sh`（tarball ~316MB，$ORIGIN RPATH）+ `scripts/install.sh`（~/.local，VFX 从 PyPI nvidia-vfx wheel curl 提取，不调 pip 不改系统）；VFX ~1.1GB 不随 tarball 分发（SLA）。

## 环境速查

- GPU：RTX 5060 Ti（Blackwell sm_120）；驱动 610.57.04；KWin Wayland 6.7.4（纯 Wayland，无 X11）
- Qt 6.11.1 / C++20 (GCC 13+) / Meson / FFmpeg 9.0 / TRT 11.2.1.2（pacman）
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
