# AGENTS.md — vsr-player 项目记忆（DeepSeek Harness 版）

> 本文件由 DeepSeek Harness 转写：来源 = 开发机上的 Claude Code 会话记忆（已筛选：保留知识/经验类 6 个主题，移除过程性/过期记录；清洗隐私/密钥后拉入仓库 `docs/memory/`）+ 原 `CLAUDE.md`（已于 2026-08 删除，信息脱节），并已对照 master 代码（`7bae9ce`）修正其中的失效/冲突信息。
>
> **本文件是项目的唯一权威指令**。原 CLAUDE.md 含未同步的过时内容与幻觉断言历史（modeset、Wayland embedding 两处已被证伪），删除后不再有冲突源；引用任何技术事实前，仍先对照代码验证（通用"证据优先/结论先验证"原则见 `~/.dsh/AGENTS.md`）。

## 项目是什么

Linux 桌面视频播放器：**NVIDIA VFX SDK 实时 AI 超分 / 插帧 / SDR→HDR10 上变换**（VSR / VFG / TrueHDR，默认链路）+ **冻结的 RIFE 插帧后端**（TensorRT，仅源码构建可选）。架构 = **libmpv**（demux/decode/AV 同步/timing/VO 全交给 mpv）+ 自定义 mpv 滤镜（`vf_hwup` 软解上传、`vf_vsr` 超分、`vf_truehdr` 上变换、`vf_vfg` 插帧）+ Qt 6/QML 前端（Vulkan，`vo=libmpv` 渲染到 Qt 场景图共享 VkImage）。很可能是首个直接调 VFX SDK（非驱动级 RTX VSR）的开源 Linux 播放器。

滤镜链：`demux → decode → [deint] → [vf_hwup] → [vf_rot] → [vf_vsr] → [vf_truehdr] → [vf_vfg] → VO (libmpv) → Qt scene graph`
（`deint` = mpv 自己的反交错 pre-filter；`rot` = 我们自己的 CUDA 零拷贝旋转，见「旋转」一节；冻结的 RIFE 构建用 `hwup → rot → rife → vsr`，链尾不加 vfg）

## 工作铁律（用户明确要求，违反会被纠正；通用方法论已迁移至 `~/.dsh/AGENTS.md`，此处仅项目特定规则）

1. **严格对齐 mpv 参考实现**——改任何逻辑前先读 `third_party/mpv/` 对应源码，列出差异再改；不自行设计、不加"优化"。mpv 没有的东西不加（除非用户明确要求）。
2. **不用 QTimer 解决渲染/事件架构问题**——业务触发器不解决架构问题；唤醒/驱动走框架生命周期钩子。
3. **字体样式冻结**——不主动改 font family/size/bold，除非用户点名。
4. **日志纪律**——默认只输出 info 及以上（`all=info`）；级别语义对齐 mpv（INFO=状态变更/VERBOSE=低频诊断/DBG=帧级）；调试用 `--msg-level all=v`（帧级 `all=dbg`），benchmark 用 `all=no`。**不要用 `log-file`**（下限 MSGL_DEBUG 会绕过 msg-level 刷屏）。
   ⚠ **日志一律用英文，不做 i18n**（用户 2026-10-22 明确要求）：日志是给 issue/bug 报告和跨语言 grep 用的，
   跟着桌面语言变会让上游反馈里出现无法检索的中文；OSD/UI 文案仍走 `tr()`/`qsTr()`。改动范围 =
   我们自己的日志（`MLOG_*`、滤镜里的 `MP_INFO/WARN/ERR/VERBOSE/DBG`、`fprintf(stderr, …)`、
   `CudaProbe`/`HdrOutput` 里拼进日志的 reason 串）；**mpv 上游自己的日志不动**（`third_party/mpv`）。
   注释仍是中文（不受此约束）。
5. **视觉瑕疵先排除编码**——任何"涂抹/色块"先出 PNG 帧序列/444 编码对照，再归因模型/引擎（yuv420p 色度下采样曾致 3 个引擎全被误判）。
6. **不假设画面里"没有 X"**（2026-10-18 用户纠正）——我们处理的是视频，逐帧逐镜头变化：**条件触发的处理项**（抗色带/去抖/去噪/去块等）必须常开，恒定成本+条件收益是设计意图；"样本上看不出差别/逐位无差异"**不构成**关闭或省略的依据。要动这类项，只能用**最坏帧**（已知有该缺陷的素材）+ 目标显示条件下的并排结果来判。
7. **UI 不自由发挥**（2026-10-18 用户要求）——做界面只用项目里**已有**的设计语言/组件/配色：弹窗用 `PopupBase`、
   两态开关用分段按钮（本项目没有 Switch）、滑块配色照 `ProgressSlider.qml`（轨道 `#44ffffff`／已填充
   `#e0e0e0`／滑块 `#ffffff` 14×14 r7）、图标用项目图标字体里**已存在**的字形（先查 cmap 再选）。
   项目里没有的形态**先和用户沟通**，不要自行发明；"仅调配色"就是只动颜色，不要顺手改几何/交互。
   ⚠ **弹窗一律不加背景遮罩**（用户 2026-10-18 要求）：`PopupBase` 里 `Overlay.modal: Item {}` ——
   调参弹窗必须随时能看到画面效果，半透明调暗层会干扰判断；`modal: true`（挡外部点击）保留。
   ⚠ **码点写成字面字符**：`\uXXXX` 只吃 **4 位**十六进制，而 MDI 码点普遍 > 0xFFFF
   （`mdi-tune` = U+F062E）→ 写 `"\uF062E"` 会解析成 U+F062 + 字母 "E"（现象：按钮里
   一个豆腐块 + E）。查图标用 `docs/icon-cheatsheet.html`（MDI v7.4.47 全表，可搜索/复制字形）。
   ⚠ **组件宽度按内容算**：`SegToggle` 内部是两个 56 宽按钮（+6 间距 = 118），调用处写
   `width: 88` 会让视觉内容**溢出到相邻控件**（实测：高级图标按钮与开关重叠）。
   ⚠ **弹窗里不写说明文案**（用户 2026-10-18）：项目其它选项也都没有解释性文字——
   只留控件标签/标题/单位；"SDK default xx"、"following panel"、状态解释句这类全部去掉，
   参数含义与默认值写在代码注释/文档里。tooltip 也保持项目风格的一句话（如 "Advanced"）。
   ⚠ **新增 UI 文案必须走 i18n**：QML 里一律 `qsTr()`；改完跑
   `./scripts/update-translations.sh`（= `lupdate -no-obsolete` + 完整输入清单），再补两份译文并
   `ninja -C build`（meson 的 custom_target 跑 `lrelease` → `<build>/src/client/*.qm`，`main.cpp`
   按 locale 经 `VSR_TRANSLATIONS_DIR` 载入；日志会打 `translation: vsr-player_zh_CN`），最后
   `./scripts/update-translations.sh --check` 验收。
   ⚠ **输入清单必须包含带 `QT_TRANSLATE_NOOP()` 的头**（当前 = `src/client/FrucBackend.h`）：
   C++ 侧用 `tr(变量)` 翻译档位标签（`frucModel`/`frucQualityModel`/`frucMaxFpsModel`），
   字符串靠 `QT_TRANSLATE_NOOP("PlayerViewModel", "…")` 标记——lupdate 没吃这个头就看不到它们，
   条目会 `vanished` → 不进 .qm → **运行时静默回退英文**（2026-10-22 用户报"UI 里插帧选项
   没做 i18n"：Off/Low/Medium/High 与各档位标签全英，就是这么漏的）。脚本的 `--check`
   会拿源码里的 `tr("…")/qsTr("…")/QT_TRANSLATE_NOOP(…)` 字面量逐个对 .ts，比数 vanished 更准。
   ⚠ 别漏 `ui/components/*.qml`：新组件（SegToggle/ParamSliderRow）是**独立 context**，
   lupdate 不给这些文件就永远没有译文。lupdate 会顺带刷新所有 `<location line=…>`（diff 大是正常）。
   ⚠ **`tr()` 的参数必须是字面量**：写成 `tr(t)`（t 是形参）lupdate 扫不到 → 该条目被标
   `type="vanished"` → **lrelease 不编进 .qm** → 运行时 `tr()` 查不到 → 回退英文。
   2026-10-18 实测（用户报"插帧的 UI 国际化没做干净"）：`osdTextString()` 的 `tag()` 用
   `tr(t)` 收标签 → 整个 OSD 标签（源/输出/超分/**插帧**/解码器…）全英文，而 .ts 里译文
   其实一直有（只是 vanished）。修法 = 在**调用点**写 `tag(tr("FRUC"))`，lambda 只做宽度对齐；
   改完必须跑 lupdate（条目会从 vanished 复活并保留原译文）+ lrelease，用
   `lconvert -i x.qm -o -` 反查确认译文真进了 .qm（**不能只看 .ts**）。


## 技术现状（已对照 master 代码修正）

### 插帧（VFG 默认；RIFE 冻结，仅源码构建）

- **引擎 = RIFE full FP16 唯一引擎**（`rife_full_fp16.engine`，动态 shape 7ch）——lite 已彻底拆除（FP16 连续推理 ~6 次后 NaN 退化，实证与 TRT 版本无关）；`--rife-model` 参数已删；`vf_rife.c` 无 variant/lite 残留。
- **模型 = RIFE 4.26 v2**（7ch `rife_v2` 变体，`third_party/rife/rife_v4.26_v2_fp16_all.onnx`）——**不是 4.25**（旧记忆仍写 4.25，过时）。切换理由：质量全档胜出（合成 GT PSNR +1~4.6dB，中位移优势最大）。构建：`bash tests/fruc/build_rife_full_engine.sh`（系统 trtexec，动态 profile FP16，`--hardware-compat on` 跨架构 ampere+）。
- **TRT 版本绑定**：引擎内嵌构建时 TRT 精确版本，升级 TRT 后必须重建引擎。当前系统 = **TRT 11.2.1.2**。
- **闪回问题已解决**（`vf_rife.c:622-694`）：整数倍率 ≥2 用 vs-mlrt `Interleave([src, interp])` 语义——源帧总保留、插值帧在相对时间点 t=i/k（tval 恒 i/k，无极端 tval）；**PTS 打在帧的内容时间**（mid 在 `t_prev + tval·(t_cur−t_prev)`、cur 副本在 `t_cur`）——严格单调、无重复无空洞（音频同步下视频间隔=源间隔/k，与音频时钟零漂移）。⚠️ 旧方案（绝对网格 `t0+n/out_fps` + llround 锚定，每对固定 k 帧）在源帧率不是网格整数倍时（如 29.97→60 比例 2.002）会周期性重复/空洞网格点 → mpv `Invalid video timestamp` + 16s 级 desync + 持续丢帧（2026-08-18 test.wmv 播放速度异常，已修）。非整数倍率（<2× 或 24→60=2.5）保留绝对网格（floor 半开区间，无重复但 VFR 源有极端 tval 风险，可接受）。根因背景：绝对网格在 VFR 源（goose 33.0/34.0ms 交替 PTS）上使 tval 摆到 ~0.01–0.99 → 极端 tval 插值 + 源帧被吞 → 内容抖动（此问题由固定 tval 内容解决，与 PTS 方案解耦）。极端 tval（<0.05/>0.95）复制近端点。
- **UI 档位 = [Off, 40, 48, 60]**（`QualityPopup.qml`）——30 已移除、48 已添加；CLI `--fruc` 接受 `off/40/48/60/2/3/4`（2/3/4 为 benchmark 倍率）。**`--fruc 30` 已不接受**。
- **VFG 输出帧率上限 `max-fps`（2026-10-20，仅 VFG）**：用户显式上限而非性能自动降级。
  `QualityPopup` 的 "Max interpolation frame rate" 排（48/60/120 fps，默认 **60**；`--fruc-max-fps` /
  RPC `set-fruc-max-fps` / 持久化键 `frucMaxFps`；RIFE 分支该排隐藏、参数忽略）。
  filter 判据（`vf_vfg.c` 直通判定）：`k × src_fps > max_fps` → `vfg-status: mode=
  passthrough reason=max-fps`；**恰好等于上限允许插帧**（24×2=48 @cap48、30×2=60 @cap60；
  已知帧率路径带 1e-6 相对容差，吸收容器帧率浮点尾数，如 goose 的 demux-fps=30.0000003）；
  **已知帧率路径的 max-fps 相对容差 1e-4（2026-10-25 修）**：容器
  `avg_frame_rate` 会有微小漂移（3D 测试片 `4024000/134133 = 30.0000745`，
  k=2 时输出 60.000149），旧的 1e-6 容差（60.00006）会把它误判成超过
  `max-fps=60`，整条插帧直通。改成 1e-4（0.01%，仍能拦住 30.1fps×2=60.2），
  并在 cap 状态变化时打 `vfg: max-fps engaged/released (src=… k=… cap=…)`。
  **src_fps 未知（直播/URL 流）时用输入帧 PTS 间隔的中位数估计**（2026-10-24 修：此前直接
  不判定，60fps 直播 + k=2 + cap=60 仍输出 120fps；现 31 样本窗口/15 预热、2% 死区 +
  30 帧进入 / 60 帧退出去抖；估计值只用于判据、不写回元数据；nominal 已知的源仍走精确值、
  不走估计逻辑——用户明确要求）；上限在引擎初始化前判 → 被限制时不建引擎/不耗 CUDA；
  源帧率变化（换片/VFR）逐帧重估，回落即恢复插帧。默认 **60**（用户
  2026-10-20 定："对于视频来说，60 已经很高了"）：24×2=48、30×2=60 放行，
  24×3/×4、60×2+ 直通；要更高由用户显式选 120。
  ⚠ 这是**显式上限**，不是把 PR #19 移除的自动降级复活——不要再往"按实测成本自动切"走。
- **VFG 竖屏慢路径（2026-10-21 实测，SDK 内部行为）**：VFX 1.3 的 VideoFrameGeneration 对
  **竖屏/窄幅输入**有一条明显更慢的引擎路径，MEDIUM 受影响最大。探针
  `tests/vfg/vfg_timing_probe.c`（纯 NvVFX API，不经过播放器；构建/用法见文件头）实测
  MEDIUM 每 Run：1920×1080 **2.5ms**、1080×1920 **8.9ms**、1920×1920 **1.8ms**、
  1920×1921 **15.6ms**、1920×2160 **17.3ms**、1920×3328 **27–28ms**、
  3808×2160 **3.3ms**、3840×2160 **3.4ms**、3328×1920 **3.1ms**。
  即：正方形最省（≈0.5ms/MP）、横屏 ≈0.8–1.2ms/MP、**竖屏 ≈4.3ms/MP（4× 以上）**，
  且 1920×1920 → 1920×1921 有 **8.5× 阶跃**（宽度≤1920 时高度越过 1920 触发慢路径；
  宽 3808 的 2160 高不受影响——判据与宽度相关，具体公式在库内部）。
  **档位顺序在竖屏会反转**：1920×3328 的 LOW **5.8** / MEDIUM **27** / HIGH **8.4**（探针），
  播放器内实测同一文件（30fps k=2、VSR 关）MEDIUM **14.7–15.3**、LOW **4.0**、HIGH **5.5 ms/输出帧**。
  `AFGX_FLOW_RESOLUTION` 是编译期常量、**不是环境变量**（试 1..64 无任何变化）。
  → 竖屏内容应避开 MEDIUM：**HIGH 更快且质量更高**（少数"免费的午餐"），LOW 最省。
  尚未做：按宽高比自动选档（属于"按局部信息决策"，需用户点头）。
- **丢帧分支必须 `mark_progress`（2026-10-22 修：坏时间戳老片源卡死）**：`vf_vfg.c` 里
  "重复/倒序/缺 PTS → 丢弃 cur、保留 prev" 的分支原先 `talloc_free(cur); return;` ——
  **消费了输入却没写出输出、也没 `mp_filter_internal_mark_progress(f)`**：mpv 的 process()
  只由 pin 事件驱动（`filters/filter.h`：filter 内消费但无输出必须手动 mark_progress），
  于是本滤镜永远不会被再次调用 → 下游永远等不到帧。现象＝**画面定格在某一帧、
  `seeking` 一直为 true、`core-idle=yes`、没有任何 `<滤镜>-status` 行、seek 后能出画面
  但播放不走**。触发条件是坏时间戳片源：实测 `/home/zmq/视频/2.avi`（实为 **Ogg 容器
  装 mpeg4/XVID 640×480**，包里有大量重复 pts 与 `pts=N/A`，mpv 打 `Invalid video
  timestamp`）在第二个重复 pts 帧上必卡；`--fruc off`（VFG 直通）与 `--scale off`
  都正常，**只有 VFG 活跃时卡**。修后全链（VSR 2× + VFG 2×）正常播放、seek 0/30/90 s
  全部 `seeking=no`、丢帧 0。对照：`vf_rife.c` 的同款分支在 `for(;;)` 预取循环里
  `continue`（不返回），所以不受影响；`vf_vsr` 的 seek 丢弃是 drop loop，同理。
- 其他机制：场景切换直通（复制时间较近端点）；预取模型（vf_vapoursynth buffered-frames，每 process() 调用输出 1 帧）；EOF flush 最后帧；模式降级时丢弃预取帧防顺序倒挂；`syncVfOptions` 在 FILE_LOADED 后重放 vf 状态（链重建后 UI 修改不丢）。
- **无性能自动降级**（2026-10-15 移除旧的分级降级：曾是"cost 超预算先降档 60→48→40→30，全档超才直通"）。移除理由：判据是**节点内自测**，只能看到 RIFE 自身 GPU 耗时，而真实短缺通常在该节点下游——VSR 按**输出帧**计费（k=out/src 次/对），加上 VO/渲染通路；实测 1080p29.97 + `fps=60` + `scale=auto` 实际上屏 17.5fps、丢帧 +52/s，而判据报 `cost=16.0ms < budget=21.4ms` 全程沉默。目前 `fruc-status` 的 `cost=` 仅作观测保留。⚠️ 移除的代价：4K 纯 RIFE 过载场景失去唯一有效的回退（24fps → 7.3fps）。**完整记录（现象/根因/同族缺陷/vf 节点能力边界/统一方案的待定问题/复现方法）见 `docs/memory/fruc-auto-degrade-removal.md`**；统一的跨节点性能自适应方案（覆盖 rife/vsr/VFX 新版插帧，以实际达成率判定）**待设计，2026-10-15 搁置**。
- 插帧历史决策：NVOFA 硬件光流降级备选（repeat 率太高，有运动对成功率 9.7% vs RIFE 96.3%）；Nano = 两帧平均器已否决（flow 恒零实证）。

### VSR（超分）

- YUV→RGB 用自研 NVRTC CUDA 内核（`yuv_to_rgba.c`，NV12/P010/P016→RGBA U8 或打包 RGB10A2，D2D 全 GPU）——旧 CPU filter graph 方案已被取代（不可再回退）。10-bit 通路见下面的「10-bit（HDR）通路」。
- `vsr_set_output` 已确认：输入不变仅输出尺寸变化**无需重建 VFX**（SetImage 换输出缓冲）；输入变化由 mpv filter 链重建覆盖。
- VFX 对 TRT **零符号调用**（纯冗余，cudnn 仅 GetVersion）——只有 RIFE 需要 TRT 11。`third_party/nvvfx/lib` 无版本 TRT 软链已改名（TRT 10/11 冲突修复），`vsr_proc.c` 用版本化 `.so.10` 加载 TRT、无版本名加载 NPP/cudnn/ngx-vsr。

### 10-bit（HDR）通路（2026-10-17）

**要点**：VFX 1.3 起支持 RGB10A2，10-bit 源不再被压成 8-bit 再进 AI；**typemap 全链 10-bit，末端仍由 mpv tonemap 到 8-bit SDR 输出**（HDR 上屏是另一件事）。完整实测（位序、探针、逐通道对账、limited-range 修正）见 `docs/hdr-pipeline-investigation.md` §17-§19。

- **开关自动**：`vfx_bits_from_imgfmt()` 按**单个分量的存储字节数**判定（NV12/RGBA=1B→8-bit，P010/P016=2B→10-bit，X2BGR10=强制 10-bit）。⚠ 不能用 `imgfmt_desc.bpp[0]`（整像素位宽，RGBA=32 会被误判 → 花屏）。
- **NvCV 常量**：`NVCV_RGB10A2=13`、`NVCV_P32=11`；`ImageEncodingMode=1` 必须在 `Load()` 前设，输入/输出格式一致。
- **位序**：打包字小端 `R bit0-9 / G 10-19 / B 20-29 / A 30-31` = `IMGFMT_X2BGR10` = `AV_PIX_FMT_X2BGR10` = `VK_FORMAT_A2B10G10R10_UNORM_PACK32`。⚠ 不是 `X2RGB10`（R/B 会互换）。
- **FFmpeg CUDA hwframes 不支持 x2bgr10** → 自建 `hw_frames_ctx` 的 `sw_format` 恒用 `RGBA`，10-bit 由帧参数 `hw_subfmt` 承载（`hwdownload` 软路径会退化成 8-bit，本应用 VO 有 interop 不走）。
- **mpv 的 ra 不认 X2BGR10** → 补丁 `src/mpv/video/out/placebo/ra_pl.c` 把 libplacebo 的 `rgb10a2` 标成 `special_imgfmt = IMGFMT_X2BGR10`（否则 CUDA interop 失败 → 4K 每帧回读）。
- **QualityLevel 语义**：1–4 = upscale 档、8–11 = 1:1 denoise 档，混用返回 -7；`vf_vsr` 的 `quality`/`denoise` 分别映射。
- **已知遗留**：① 4:4:4/4:2:2 hw 帧被 kernel 当 4:2:0 读；② `-Dfruc_backend=rife` 分支冻结在 8-bit（VFG 链才走 10-bit）。~~软解 10-bit 偏暗~~ 已证伪（同机逐像素 mean|Δ|=0）。

### HDR 上屏（2026-10-17 落地，细节见 `docs/hdr-pipeline-investigation.md` §10-§22）

**链路**：内容 HDR（mpv `video-params/gamma` ∈ pq/hlg）× 显示能力 HDR → 切 HDR10（rtImage / composite attachment / mpv wrap 全走 `A2B10G10R10_UNORM_PACK32`，mpv overlay 填 `ra_fbo.color_space`），否则保持 SDR。

- **只对 Vulkan RHI 生效**：Wayland 下 Qt 默认 OpenGL 时 `_qt_sg_hdr_format` 被静默忽略；本项目自建 Vulkan instance + 该动态属性。
- **色彩空间声明**：`QSurfaceFormat::setColorSpace(Bt2020+ST2084)` + 自建 `wp_color_management` 模块（`WaylandColorFeedback.cpp`）下发 parametric 描述；⚠ 一个 wl_surface 只能有一个 color management surface（show 前设 colorSpace 会与模块冲突 → 连接断开），所以描述一律在首帧后由 `syncTargets()` 下发。
- ⚠ **判据不能用 Qt**：`QRhiSwapChain::hdrInfo()` 在 Linux 是 Qt 硬编码默认值（1000nit/200）；`isFormatSupported()` 在 Wayland 是"合成器整体支持"（SDR 屏也 true）。**唯一可靠来源 = surface preferred 描述**：`gamut_area > 1.15×sRGB(0.112)` 或 transfer 是 PQ/HLG；读不到一律退 SDR。
- ⚠ **PQ 下 `set_luminances` 的 max_lum 被协议忽略（恒 min+10000）** → 内容峰值必须经 `set_mastering_luminance` + `set_max_cll`/`max_fall` 下发（来源 mpv `video-params/*`，合法性照 `cm_metadata_sanitize()`）；缺了它 KWin 按 10000nit 压 → 对比度变平。
- **目标色彩空间必须由宿主用 mpv 选项声明**（`target-trc=pq` / `target-prim=bt.2020`）：generic libmpv 路径自己拼 `ra_fbo` 会丢掉 backend 的 `color_space`，gl_video 把 PQ 烘成 gamma2.2 → 偏色。**不要**把合成器亮度填进 `ra_fbo.color_space.hdr.max_luma`（会预 tone map 到 203nit）。
- **`VSR_HDR_DECLARE=0`（不自己声明）在本机画面发灰**，默认 1；`VSR_HDR=off|auto|hdr10` 默认 auto，`hdr10` 仅用于诊断/逃生舱（SDR 内容会更暗），不暴露 UI。
- **OSD**：`HDR` 行 = 内容侧（`SDR · content SDR` / `... · content 758nit` / `SDR · content HDR not displayed (<原因>)`），`Output` 行 = 真实 swapchain 格式（`· HDR10` / `· SDR`，取自探测而非策略意图）。
- **⚠ 运行期切换的已知坑**：`applyMode` 里模式未变也要 `syncTargets()`（幂等），否则漏设 `target-trc`；`CompositeRenderNode` 必须在渲染阶段从 Video 读 pipeline（缓存句柄会绑定已销毁对象）；SDR↔HDR10 切换仍是同步属性写，软解 + DR 等帧时理论上仍可能死锁（彻底修法 = 异步写）。

### SDR→HDR10 上变换（VFX TrueHDR，2026-10-18 落地，**默认关**）

**位置**：链上恒在 `vf_vsr` 之后（`hwup → vsr → truehdr → vfg`；`--fruc-order pre` **不插** → 记日志）。链串恒带占位 `@truehdr:truehdr:enable=no`（⚠ 不能写 `enable=0`，`OPT_BOOL` 只认 yes/no，写错整条 vf 解析失败）。开关 = 内容 SDR ∧ opt-in（`VSR_UPHDR=on` 或 UI）∧ 策略≠off ∧ 链上有滤镜；过滤器另有按帧 transfer 自守卫。细节见 `docs/vfx-truehdr-investigation.md` §8。

- **参数**（5 个，区间是硬约束且越界**静默失败** = SetU32 返 -7 但 Load/Run 仍 0，值不生效）：`Contrast` 100(0-200) / `Saturation` 100(0-200) / `MiddleGray` 50(**10-100**) / `Luminance` 650(**400-2000**，= 面板物理峰值) / `DebandingOff` 0（恒 0，抗色带常开）。三处区间夹取同步：`TrueHdrParams.h`、`vf_truehdr.c` 的 `M_RANGE`、`HdrOutput` 的 clamp。
- **热更新**：参数走 `NvVFX_SetU32`（实测与 destroy+create 逐位相同）；仅帧尺寸变化 / `DebandingOff` 变化 / SetU32 失败才重建引擎（≈25ms）。
- **输出**：`IMGFMT_X2BGR10` + `pl_color_space_hdr10` + `hdr.max_luma = Luminance`（对齐 mpv `vf_d3d11vpp.c`）；上变换生效时给合成器的内容峰值也用该 Luminance。成本 4K ≈ 4.9–5.8 ms/源帧。
- **UI**：`QualityPopup` 只放开关 + Advanced（`mdi-tune`）→ `TrueHdrAdvancedPopup` 四条 slider + Reset、Debanding 两态；Reset = SDK 默认，Luminance 例外（回面板峰值）；Luminance 有 Auto/Manual 覆盖（Auto 跟面板）。持久化键 `upHdrEnabled/LumOverride/Luminance/Saturation/Contrast/MiddleGray/Debanding`。
- ⚠ **同步 mpv API 死锁**：`mpv_set_property` 等 core dispatch lock，软解 + `vo=libmpv` 直渲时若 core 卡在等 DR 图像会循环死锁（gdb 实证）。已修：`setHdrTarget` 幂等 + 首帧 `reevaluate` 排队到 GUI 线程；**软解片源做 HDR 切换验收必须覆盖这一格**。

### 渲染循环（Qt Quick）

- **纯请求驱动**：mpv update callback（按帧率推送；无活动时完全静默）→ QueuedConnection → 条件 requestRender（`mpv_->update() > 0 || renderRequested_`）→ updatePaintNode → render + reportSwap + vkQueueWaitIdle。**无心跳/无自主循环**——旧文档的"200ms 心跳"模型已证伪。
- `QSG_RENDER_LOOP=basic` 必须（threaded 每帧渲染饿死事件循环）。**绝不无条件 update()/postEvent**（渲染风暴回归教训）。`aboutToBlock` 兜底已删除（`Video.cpp:377` 注释确认）。
- 卡死归因：目录播放概率性卡死根因 = filter 每帧 `get_render_target_size` 持锁等渲染线程 → vo_libmpv.c 无锁读（__atomic relaxed）已修复。
- **Xid 109**（全屏切换 device loss）：根因 = `Video::ensureRenderTarget` 销毁 rtImage 前无 GPU 同步（Vulkan UAF）→ 销毁前无条件 `vkQueueWaitIdle` 修复（`tests/fs_stress.sh` 验证）。
- ⚠ **GUI 线程（= 渲染线程）不得同步调 mpv API**（`mpv_get_property` / `mpv_set_property` / `mpv_command` 都要等 core 的 dispatch 锁）：软解 + `vo=libmpv` 时 DR 帧分配**只由 GUI 线程**在 `mpv_render_context_update()` 里服务（`vo_libmpv.c:443`），GUI 一旦阻塞 → 解码线程等 DR、core 卡在 `send_packet` → 双向死锁（窗口完全无响应、只能杀进程）。**2026-10-24 实证**：v0.4.3 的 path 观察器回调里 `refreshCanDeleteCurrentFile()` 同步重读 `path`，软解启动 1080p h264/hevc 必挂（gdb：GUI 在 `mp_dispatch_lock`、core 在 `send_packet`、解码线程在 `dr_helper_get_image`）。规则：GUI 线程的属性读用观察器缓存/事件线程读，写与命令一律 `*Async`；启动/idle（core 空闲、无解码线程等 DR）的同步调用可保留（`restorePlaylist*`/`resumeLastPath`）。
- **损坏文件的硬解回退 = mpv 原生行为，接受**（2026-10-24 用户定）：`hwdec-software-fallback` 默认 3 连错（`AVERROR_INVALIDDATA` 也算）→ `force_fallback()` 永久软解；实测 h264 空洞冻结 9.2s（新软解器等关键帧）vs 保持硬解 4.1s。mpv 判据不区分"码流坏/硬件坏"，属上游设计；不做本地补丁，也不改 UI 的 "software-only" 判定（回退后按钮仍禁用，重开文件即恢复硬解）。


### 启动与会话行为（2026-10-17）

- **命令行输入 = 播放列表（mpv 语义，2026-10-18）**：位置参数与 `--playlist <file|->` 按**出现顺序**登记（`Options::Input`）；`--playlist` 的内容在展开时插入它出现的位置。多条 → 首条 `loadfile`（replace + 播放）+ 其余 `loadfile … append`（`MpvController::appendFile`）。播放列表文件 = m3u/纯文本（跳过空行与 `#` 注释，去行尾 CR），`-` = 从 stdin 读到 **EOF**（ff2mpv wrapper 的 `--playlist=-` 就是这条；交互式终端下别用，会一直等）。给了输入就**不**走 idle 恢复、也不恢复上次列表；展开后为空 → 不载入媒体（对齐 mpv 空列表 → idle）。展开在读入时做（`main.cpp: expandInputs()`），`Options::parse` 保持纯字符串解析、不做 I/O。
- **无参数启动 = idle**：只恢复上次播放列表（`restorePlaylistIdle()` 全 `loadfile … append`，mpv 无 current 条目 → 不解码、不建滤镜链、不 Load 引擎、不做续播 seek），按播放才 `resumeLastPath()` → `playlist-play-index` 载入，mpv 用 watch_later 自动恢复上次进度。命令行给了文件、或显式 `-- --pause=yes` → 立即载入（mpv 语义）。
- 起因：曾有"启动暂停"实现（`pause=yes` 选项 + 启动即载入），但 mpv 精确 seek 会把"关键帧→目标位置"的帧**过完滤镜链再丢掉**（实测 `start=12.302`/关键帧 6.25s → 142 源帧、VFG 570 输出帧白跑、海报晚 1.6s）；日志里那批 `vfg-status` 是丢弃帧不是播放。完整测量与被否方案：`docs/memory/startup-idle-policy.md`。
- idle 时 mpv 的 `pause` 初值为 false → `pause` 观察器按 `updatePlaying(!p && fileLoaded_)` 判定，否则 UI 未载入就显示"播放中"。
- **打开文件夹 = mpv 原生目录展开（2026-10-21）**：`openFiles` **不扫描目录**，目录路径原样交给 mpv `loadfile`（`main.qml` 的 FolderDialog 用 mode 0，与拖入窗口的 DropArea 同一条通路）。mpv `demux_playlist` 按 `--directory-mode`（默认 lazy）与 `directory-filter-types`（默认 video/audio/image/archive/playlist）展开：顶层媒体成条目（文件在前、目录在后，`mp_natural_sort_cmp`）、**子目录作为条目留在列表**、点击时再展开（`playlist-play-index` 后目录条目原地替换为其内容，`PlaylistModel` 前缀 diff 照常处理）。旧实现（mode 3）用 Qt `QDir::entryList` 只扫顶层固定扩展名表 → 子目录/图片/播放列表全丢，与拖放不一致，已删除。证据：测试目录（2 媒体 + 1 子目录 + 1 软链目录 + txt/srt）装载后 mpv 与播放器均 4 条目，点目录条目展开为其中文件。
- **播放列表面板"空白"根因与修法（2026-10-21）**：`PlaylistPanel` 是 `Drawer`（Popup）。关闭时 popup 内容从场景树摘除；**关闭期间**播放列表增删（换片/追加/目录展开）后，ListView 的 delegate 保持旧几何（实测 120→20 条重开后 delegate y 仍是 4326..4872，而 contentHeight=840）→ 视口内没有任何条目 = 面板空白，且**不自愈**（第一次滚轮才跳回）。修法：ListView 放进 `Loader`，`onOpened` 创建 / `onClosed` 销毁（每次打开都是全新布局）；另加 `clampScroll()`（`contentHeight/height/count` 变化时 `Qt.callLater` 夹 contentY，`boundsBehavior: StopAtBounds`）与滚轮 handler 对**全部** wheel 帧 `accept`、写入前 `cancelFlick()`（避免手写 contentY 与 Flickable 的 wheel flick 两套滚动互相打断）。验证：修复前重开截图为空，修复后显示 20 条；120 条滚到底 contentY=4368、末条 120 可见。新 ListView 全新建局 → 每次打开都会重建；`onLoaded` 里 `positionViewAtIndex(currentIndex, ListView.Center)` 把当前播放条目居中（实测 index 61/120 → contentY=2205，前后各 ~16 条可见），无 current（`currentIndex<0`）时回到顶部。
- **两个拖放区（2026-10-21 确认）**：A = 窗口级 `DropArea`（`main.qml`）→ mode 0，**完全重置**播放列表（首条 replace 播放、其余 append 排队）；B = `PlaylistPanel` 的 `DropArea` → mode 1，**追加**（不打断当前播放）。面板打开时 B 在 popup overlay（`QQuickOverlay` 是 contentItem 的子项、创建在 QML 根之后）里叠在 A 之上，Qt 的拖放投递按 `paintOrderChildItems()` **从上到下找第一个接受者**（`qquickdeliveryagent.cpp: deliverDragEvent`），所以两个区不会同时触发。**追加目录**同样走 mpv 原生语义：`loadfile <dir> append` 只留一个目录条目、点击才展开（实测：追加后列表 = `[/tmp/vsr-dir-test]`，点击 → 4 条目并播放首条）。
  菜单侧同语义（2026-10-21）：右键菜单 = "Add files to playlist"（FileDialog mode 1）+ "Add folder to playlist"（FolderDialog mode 1，同一 `folderDialog` 实例靠 `mode` 属性区分标题与 `onAccepted`）；"打开文件/文件夹"仍分开（平台对话框不能同时选文件与目录），但两者与追加共用 `openFiles` 单一入口，目录路径一律交给 mpv。


### 当前条目的移除 / 删除（2026-10-23）

两类动作共享同一套"前进"语义，**只差一条 `gio trash`**：

| 动作 | 快捷键 | 菜单项 | 是否动磁盘 |
|---|---|---|---|
| 从播放列表移除当前条目 | `Delete` | Remove from playlist（从播放列表移除） | 否 |
| 删除当前文件 | `Shift+Delete` | Delete file（删除文件） | 移到回收站 |

**前进语义全部由 mpv 的 `playlist-remove` 决定**（`player/command.c: cmd_playlist_remove`
→ `common/playlist.c: playlist_remove`；2026-10-23 实测四条）：

- 移除**当前**条目：`current` 指向后一条 + `PT_NEXT_ENTRY` → mpv 自己载入下一个；
- `pause` 是**全局属性、跨曲目保持** → 暂停状态下新条目载入但不播放（`time-pos` 不动），
  正是"当前暂停时下一个不立即播放"；
- 没有后继且 `loop-playlist=no` → `count=0 / pos=-1 / idle-active=yes`（停止并保持 idle）；
- `loop-playlist=inf` 且移除最后一个 → 回到剩余第一条继续（不 idle）。

因此客户端只发一条 `playlist-remove <playlist-pos>`，**不自己判断"有没有下一条/要不要停"**。
实现：`PlayerViewModel::removeCurrentFromPlaylist()`（读 `playlist-pos`，-1 则忽略）、
`deleteCurrentFile()`（先 `gio trash -- <path>`，成功才移除；失败保持列表不动并记 ERR）。

**可删条件**（`canDeleteCurrentFile`）：`path` 非空、不含 `://`（URL/流没有"文件"可删）、
`QFileInfo` 为常规文件且可写；相对路径按当前工作目录解析。菜单里 delete 项用
`hasMedia && canDeleteCurrentFile` 控制可见（`ContextMenu` 新增 `visibleWhen` 支持，
隐藏行**不占高度**，二级菜单 `subY` 的累加也要跳过隐藏行）。

⚠ **移除/清空后不能"复活"（2026-10-23 用户实测报的 bug）**：`play()` 的
`!fileLoaded_` 分支会走 `resumeLastPath()`，而 `resumeLastPath()` 在列表里找不到
`lastPath_` 时执行 `loadfile <lastPath_> append-play`——于是"移除最后一个条目/清空列表"
之后点播放，**刚被移除的文件又被追加回列表并播放**（现象：画面只是停住，再点播放又回来）。
修法：`playlist` 观察器在**列表变空**时清 `lastPath_` / `idlePlayIndex_` / `videoInfo_`，
并让 `play()` 在"既无 lastPath_ 也无 idlePlayIndex_"时直接返回。

⚠ **`playlist-clear` 不真清空**：mpv 的 `cmd_playlist_clear` 注释写明
"Supposed to clear the playlist, **except the currently played item**"——它保留当前项
（实测：2 条 → 清空后 count=1，当前文件继续播）。所以 `playlistClear()` 先
`playlist-clear`（其余条目删掉、当前项归位 index 0）再 `playlist-remove 0`，
才得到 count=0 + idle。面板的"清空"与条目 X 都走这条路径（X = `playlist-remove <index>`，
作用于当前条目时与菜单"从播放列表移除"完全同一条命令）。

⚠ **`gio trash` 在 tmpfs（`/tmp`）上会失败**（"不支持在系统内部挂载上的丢弃到回收站操作"）
——此时按设计**不改播放列表**（文件还在），只记 ERR。测试要用 `$HOME` 下的文件。
⚠ RPC 同步提供 `remove-current` / `delete-current`（测试与脚本用）。


### 快进/快退（连续 seek 门控，2026-10-21）

- **键位**：Qt Quick 自己处理（`main.qml` 的 `Keys.onPressed`，**从不把按键喂给 mpv 的 input 层**）：`←/→` = ±5 s，`Shift` = ±10 s；mpv 默认是 `seek ∓5`（相对、**关键帧**精度）+ `Shift` = ±1 s exact + `↑/↓` = ±60 s（`third_party/mpv/etc/input.conf`，编译时嵌入）。
- **mpv 的既有机制**：`cmd_seek` 恒带 `MPSEEK_FLAG_DELAY`；`execute_queued_seek()`（`player/playloop.c`）只在"上次 seek 起 0.3 s 内且还没出帧"时延后，超时就丢弃在途 seek 按最新目标重启；相对 seek 在 `queue_seek()` 里**累加**。`--hr-seek` 默认值 2（`default`）= **绝对 seek 走精确（HR）、相对 seek 走关键帧**。
- **按住不放的真实行为（实测，720p av1 470 s 素材）**：mpv 自带（IPC `keydown RIGHT`，默认 200 ms/40 Hz）1 s 内执行 34 次 seek、只出 **15 帧**、位置 +224 s（一次跳 12–43 s）；25 Hz/600 ms（= KDE 默认重复率）2 s → 36 次 seek / **29 帧** / +255 s。本项目同 25 Hz、无滤镜：50 发 / **29 帧** / +245 s → **两者同量级**，即"按住 = 整段跳画面"是 mpv 语义本身，不是我们跑偏。滤镜一开（VSR 2×）单次 5 s 精确 seek 要 **200–770 ms**，于是 2 s 内只出 1 帧、位置跳 30–40 s。
- **当前实现（2026-10-21，含 bug 修复）**：完成信号 = **`MPV_EVENT_PLAYBACK_RESTART`**（`main.cpp` 事件线程 → `PlayerViewModel::notePlaybackRestart()`），**不能用 `seeking` 属性观察**——libmpv 对观察属性做**值去重**（`player/client.c: send_client_property_changes()` 只在当前值与上次下发的不同时才发事件），连续 seek 时 true/false 互相抵消，最后一条完成通知可能根本不送达。实测症状：**拖进度条（一次拖动会连发多个绝对 seek）之后再按方向键永久卡在同一位置**（`seekInFlight_` 卡住，5 s 兜底也会被后续按键不断重置 `seekIssueMs_` 而失效）。事件流不去重、逐次到达；回调里再用 `seeking` 复核一次（新 seek 已开始 → 忽略迟到的旧完成事件）。`seekAbsolute()` 发出即置位 + 记 `seekIssueMs_`；`seekRelative()` 在途期间**直接丢弃**本次步进（不排队、不累加）。兜底：5 s 超时放行（不可 seek 的流），`isLiveSource()` 直播流不参与门控。
- **验证**：VSR 2× + 25/60 Hz 连发 2 s → 50/120 次请求里只执行 5 步、每步严格 +5 s（samples 51.1→56.1→61.2→66.2→71.2→76.3），此前同样条件为 1 帧、跳 30–40 s；无滤镜 25 Hz → ~27 步/2 s（≈ 重复率上限，与 mpv 的 29 帧一致）。间隔 100 ms 的两次点按只前进 5 s（第二次在途被丢弃，符合"过程中的 seek 抛弃"），间隔 900 ms 两次 = +10 s；暂停态步进、后退、恢复均实测正常。**回归**：进度条拖动 burst（10 次绝对 seek @15 ms，含重复值）后 +5 正常（修复前卡死）；绝对 seek 后 30 ms 内的 +5 仍被正确丢弃（不会提前放行）。
- **注意**：这是**客户端门控**，不是 mpv 内置能力（mpv 自己的 0.3 s 合并窗口做不到"每步出帧"）。`currentTime_` 仍是乐观更新（发出即更新 UI），门控保证它和落点一致，因为精确 seek 落在目标时间戳上。
- **seek 固定开销的构成（2026-10-21 实测 + 修复）**：1080p 29.97 素材、精确（HR）绝对 seek，**耗时与跳多远无关**（+2/+10/+30 s 同值）——mpv 的 hr-seek framedrop 在解码器层丢掉目标前的帧，滤镜不处理它们。分配置中位数：无滤镜/`set-vsr off` **35 ms**、仅 VFG(k=2) 42 ms、仅 VSR 330 ms、VSR+VFG 366 ms。日志每次 seek 都有 `vsr: reset (engine destroyed)` → 根因是 **`vf_vsr.c` 的 `f_reset` 每次 seek 都 `vsr_destroy`（销毁引擎）+ 下帧重建**；`vf_vfg.c`/`vf_rife.c` 的 `f_reset` 明确**保留引擎**（"重新 Load 代价高"）。修法 = vf_vsr 的 `f_reset` 同样保留引擎（只清 `frame_count`/`status_emit`），位深/尺寸变化仍由 `ensure_vsr` 触发重建、CUDA context 变化仍由 `ensure_cuda` 处理；顺带把 `vsr: init` 从 VERBOSE 提到 INFO（与 `vfg: init` 一致）。**效果：仅 VSR 330→180 ms、VSR+VFG 366→220 ms**。剩余 ~145 ms **不在 vf_vsr 内**（临时插桩实测：reset 后首帧 3–4 ms 到达、单帧处理 6 ms；而 VSR off 只要 35 ms）→ 在滤镜之后的 VO/渲染路径（VSR 输出 2560×1440 CUDA 帧）。
- **两个被实测否掉的"优化方向"（2026-10-21，用户提出）**：① **seek 期间暂停播放**——不符合 mpv 用法（mpv seek 时内部就是 seeking 状态：不显示新帧、清音频缓冲，但**不改 `pause` 属性**），而且实测**暂停态 seek 与播放态同值（~177 ms）**，无收益；副作用是 `pause` 观察器/播放图标/watch-later 状态抖动。② **seek 期间 VSR/VFG/TrueHDR 全直通**——省不掉那 145 ms（它不在滤镜里），且 VSR 直通=输出尺寸 1×（vs 1.33×/2×）→ 滤镜链 + `vo=libmpv` 共享图/合成纹理每步重建，还会显示未超分帧；引擎重载那部分已经用"保留引擎"从根上解决。
- **seek 目标之前的帧被喂进滤镜链 → 已修（2026-10-21，把 seek 从 212 ms 降到 ~40 ms）**：A/B（1080p29.97、精确 seek、VSR+VFG）：VSR auto **212–216 ms**、`set-vsr off` **40–47 ms**、固定 `--scale 2` **265 ms**；无音频同样 207–227 ms（排除音频）。**不在 VO 侧**：`VSR_LOG_VIDEO=1` 逐帧日志显示 seek 期间只渲染 2 帧且 `rnd≈0.2 ms`（无新帧）。**根因**：`vf_vsr` 里临时打印 `mpi->pts` 看到——seek 到 45 s 后滤镜处理的是 **pts=40.04、40.11…**（demux 落到的关键帧、目标之前）的帧，每 6.2 ms 一帧连续进 VSR 直到越过目标；这些帧最终在 VO 被丢弃（画面正确）但 VSR 白跑。`hr_seek_framedrop` 只在解码器层按 packet 丢，对这条链不生效/不充分。**实现**：`vf_vsr` 加 `skip_before_pts`（`vf-command vsr seek-target <秒>`，0=清除）；`f_process` 在**同一次调用里循环拉取**丢弃 `pts < target-5ms` 的帧（早先每帧 `mark_progress`+return 绕图一圈，实测每帧框架开销 ~0.3 ms；改成 drop loop 后单帧只剩解码成本），首个 `pts >= target` 的帧照常处理，但**标记不在此清除**（见下一条）。⚠ 若不 `mark_progress`/不循环直接 return，下游取帧请求落空 → 整条链卡死在 `seeking`（踩过）。客户端 `seekAbsolute()` 在 seek 命令**之前**发该命令；**不要在 `notePlaybackRestart()` 里从事件线程补发清除**——跨线程的 async 命令顺序不保证，清除可能插到下一步的 marker 之后把那一步的跳过抹掉；滤镜侧只在 3 s 过期 / 下一次 seek-target 时清，直播流客户端不发 marker。**效果**：VSR 开启时精确 seek 典型 **40–130 ms**（原来 212 ms；按住 25 Hz 实测 ~9 步/3 s vs 修复前 ~2.5 步/3 s，VSR 直通上限 ~15 步/3 s）。
- **marker 必须留到过期，否则"偶发特别慢"（2026-10-22 实测更正）**：一次 seek 里 mpv 会把目标之前的帧**喂两遍**——第一遍驱动到目标（marker 生效，drop loop 丢 297–450 帧），随后 `vsr: reset`（链 reset）之后再从更早的位置喂 60–130 帧（0.5–2 s 的量）。早先实现"首个 >= 目标的帧就清 marker"，这批迟到帧便逐帧进引擎（2.8 ms/帧）→ 单次 seek 多 **200–400 ms**，正是按住方向键时"连续几次快必有一次特别慢"。**修法**：marker 只在 3 s 过期或下一次 `seek-target` 时清除，正常播放零影响（每帧只多一次 pts 比较）。**实测（catlove_470s，60 fps，VSR auto 2×）**：单步中位 **53 ms**（p90 137 / max 167），按住 25 Hz 3 s → **29 步 / +148 s**（修前 12 步 / +64 s），与 `set-vsr off` 的 29 步 / +149 s 持平；60 Hz → 33 步 / +168 s（VSR off 46 步 / +234 s）。⚠ 之前"剩余慢步 = 精确 seek 固有解码成本（VSR off 同样慢）"的结论**作废**：那次 VSR off 的 324 ms 也是迟到帧 + 当时的系统负载所致，与 VSR 无关。
- **不改用关键帧 seek（实测否决）**：mpv 原生相对 seek（`seek +5 relative`，即其方向键语义）在按住 25 Hz 时**累加**目标，3 s 内位置 +439 s（画面整段飞过），60 Hz 直接冲到文件尾并回绕；它的"每帧看着正常"只是因为画面在整段跳。门控 + 精确绝对 seek（每步严格 +5 s、落点精确）在修好 marker 之后速度已与关键帧同级，因此保留现语义。

### 旋转（vf_rot，CUDA 零拷贝，2026-10-25）

- **链上位置**：`hwup → rot → vsr → truehdr → vfg`（VFG 链；RIFE 链为
  `hwup → rot → rife → vsr`）。`@rot:rot=angle=N` 常驻链串（angle=0 直通），
  运行期 `vf-command rot angle N` 只改实例选项、不重建链；持久化键 `rotMode`
  （0|90|270；180 只给 vf-command/手工用，UI 不暴露）。
- **实现**（`src/mpv/video/filter/vf_rot.c`）：NVRTC 编译 `rot_u8/u16/u32` 三个
  kernel，直接读写 `IMGFMT_CUDA` 的 NV12/P010 平面——**零拷贝**，无
  hwdownload/hwupload。Y 平面按元素宽 1/2 字节；UV 平面按 **pair** 整体搬运
  （NV12 一对 U/V = 2B、P010 = 4B），否则 U/V 会互换。输出帧新 cuMemAlloc，
  hwctx = **旋转后尺寸**的 CUDA/NV12|P010 frames ctx（`mp_cuda_ensure_frames`；
  hwdownload 按 hwctx 尺寸解析，借用源尺寸会错乱/崩溃）。上下文借上游共享结构
  或 hwdec 帧（cuda_shared.h 同一套）。
- **语义**：90/270 为顺时针（270 = 逆时针 90°），90/270 交换 w/h 与 p_w/p_h；
  文件 rotate 元数据原样带出（菜单角度**叠加**在它之上，与 `--video-rotate` 一致）。
- **UI**：右键菜单 "Rotate" 二级面板 Off / 90° / 270°（与反交错共用同一个二级
  面板机制，`subIndex` 区分展开项）；RPC `set-rotate 0|90|270` / `get-rotate`。
- ⚠ 旋转在 VFG **之前** → 横转竖会让 VFG 输入变成竖屏（如 720x1280）走慢路径
  （见插帧一节的竖屏实测），竖转横则相反。这是"VFX 之前旋转"的固有代价。

### 3D 输出（OSD + 视频映射，2026-10-25，分支 feat/stereo-osd-prototype）

- 入口/流程：右键菜单 3D 三态 **Off / 3D片源 / 2D转3D**（运行期、不持久化，默认 Off）。
  3D片源 → 选择器（QScreen 列表，名称+实际分辨率；**不假设 1080×2/不按宽高比猜眼镜**；
  屏幕记忆只做默认选中，不自动切屏/启动）→ Start。2D转3D 本期只弹提示，不启动。
- **主 UI 始终在桌面**：`main.cpp` 另建一个 `QQuickView controlView`，加载同一份
  `main.qml` 但 `controlMode=true` → `Video` Loader 不创建，只有 UI。2D 时它隐藏、
  主窗口（Video+UI）在桌面；3D 时主窗口去眼镜屏做 presentation，controlView 在主窗口
  原来的屏幕/几何上显示，主 UI 不消失。3D 快捷键只在 control 窗口启用（两个窗口都载入
  main.qml，避免 `Qt.ApplicationShortcut` 重复触发）。
- **presentation** = 主窗口（Video/VO）：`setScreen(眼镜屏)` + `showFullScreen`，加
  `FramelessWindowHint | WindowStaysOnTopHint | WindowDoesNotAcceptFocus`，光标 Blank；
  主窗口 uiLayer 在 3D 时隐藏；OSD 是 presentation 内的 `StereoOsd.qml` 覆盖层
  （Loader，z=100），不是独立窗口。`Tab` 只切覆盖层；`J/K/L` 改布局/眼序/全半。
  实测 KWin 下满屏置顶覆盖了任务栏/面板，无其他可见内容。停止时恢复主窗口 flags/
  光标/屏幕/几何，隐藏 controlView。
- **屏拆分在 VO/合成阶段**：`vf_*`（VSR/VFG/TrueHDR）链路不动，滤镜仍处理完整
  源帧；`CompositePipeline` 用 push constants 把输出窗口左右半映射到左右眼。
  实测 VSR 仍 active（scale 随 presentation 窗口自动变 1.5），VFG 的
  passthrough 是既有 `max-fps` 判定，不是 3D 改动。
- **映射语义**（用户 2026-10-25 定）：SBS=左右，TAB=上下；`L` 按 split 轴切
  **全/半**（SBS 全宽/半宽，TAB 全高/半高）。源眼画面保持自身比例，在“全/半 box”
  内做 contain 并居中；全/半只决定 box（SBS 调宽、TAB 调高），不拉伸。目标屏按实际
  尺寸；输出 framebuffer 恒 SBS，因此**眼视口 aspect = outAspect/2**（曾误用
  outAspect 导致画面窄一半，已修）。源帧在 rtImage 里是 mpv letterbox 后的内容，
  先算 contentRect 再取样。无红绿/anaglyph。
- OSD：默认显示；`Tab` 显隐；`J` 布局、`K` 交换眼、`L` 全/半；标题/键名 Bold、
  正文 DemiBold；左右面板 ±12 logical px 交叉视差（OSD 浮在画面前，独立于视频眼序）。
- 不做“切回 2D 自动结束 3D”（目标不一定是 1080×2，无法可靠判断）；只有目标屏被
  拔掉时 stop。`Esc` 不参与 3D。
- 测试钩子（RPC）：`stereo-start <1> <screen-index>`、`stereo-stop`、
  `stereo-osd <0|1>`、`stereo-cycle layout|swap|half`、`stereo-picker <1>`、
  `grab-window <png>`。
- 待做：i18n、2D→3D、多轨/其他 3D 格式、control UI 视频区域（当前黑底，视频只在
  presentation）。

### 反交错（mpv 原生 `--deinterlace`，2026-10-22）

**mpv 怎么做的**（全部在它的输出链里，**不经过 `--vf`**）：

- 选项 `--deinterlace=no|yes|auto`（默认 **no**）+ `--deinterlace-field-parity=tff|bff|auto`
  （`options/options.c:467` 的 `filter_conf`）；同时列在 `watch_later_options`
  （`options/options.c:1174`）→ **按文件记忆**。
- 实现 = 输出链里固定的 `deint` **pre-filter 节点**（`filters/f_auto_filters.c:56`，
  插入点 `filters/f_output_chain.c:705`）。链序 `pre_filters(userdeint) →
  user_filters(--vf) → post_filters(autorotate)`，即
  **`decode → deint → hwup → vsr → truehdr → vfg → VO`**。
- 反交错器按像素格式选（`f_auto_filters.c:100-160`）：`IMGFMT_CUDA`（我们
  `hwdec=nvdec`）→ **`bwdif_cuda`**（全程显存，不回读）；`IMGFMT_VULKAN` →
  `bwdif_vulkan`；VDPAU/D3D11/VAAPI → 各自 VPP；其余 → 自动转软件格式 + `bwdif`。
- 参数 `mode=send_field` → **每个场输出一帧 → 帧率翻倍**（25i→50p）；`yes` 时
  `deint=0`（逐帧都处理），`auto` 时 `deint=1`（bwdif 按帧标志决定）。
- `auto` 判据 = `img->fields & MP_IMGFIELD_INTERLACED`（ffmpeg 解码器给的标志）。
  滤镜一旦插入就保持（`|| p->sub.filter`），只有像素格式变化才销毁重建；文档明确
  警告 `auto` 会误判（标了隔行但其实逐行）。
- 运行期切换 = `set options/deinterlace no|yes|auto`（`options/<name>` 是 mpv
  暴露选项的通用属性路径，`DOCS/man/input.rst`）。`deint` 节点每帧
  `m_config_cache_update()` 读值，`no` 立即 `mp_subfilter_destroy()`；选项
  change flag = `UPDATE_IMGPAR`（只 reset 解码器参数 + 强制刷新），**不重建用户
  滤镜链**（VSR/VFG 引擎保留）。只读状态 `deinterlace-active`（`player/command.c:1441`）。

**本项目的接入（三态开关，右键**二级菜单**）**：

- 语义 = mpv 的三档，**不是** toggle：`0=Off(no)` / `1=Auto(auto)` / `2=Force(yes)`。
  `PlayerViewModel::deintMode`（int 属性）+ `setDeintMode()`（RPC
  `set-deinterlace off|auto|force` 走同一条路径，`get-deinterlace` 读回）。
- UI：右键菜单 `ContextMenu` 的"Deinterlace"条目 → 右侧二级面板（Off/Auto/Force，
  当前项打勾 ▸ 用 `mdi-check`，条目**不配图标**——用户 2026-10-22 定）。菜单条目
  支持 `submenu: [{label, value}]` 的一般机制见 `ContextMenu.qml`。
- **二级面板画在同一个 Popup 里**（打开时 popup 加宽、面板占右半边：
  `width: menuWidth + (subOpen ? gap + subMenuWidth : 0)`，背景用两条 Rectangle
  分别画主菜单/面板）。⚠ 不要改成嵌套 Popup：两个 popup 各自抢 grab，点面板会被
  外层判成"点击 outside"→ 菜单自己关掉。`showAt()` 的横向钳制按**展开后的宽度**
  算，否则面板会顶到屏幕外。
- 时机：`loadSettings()` 读 `deintMode`（夹 0..2）→ `applyPlaybackSettings()` 下发 →
  **`FILE_LOADED` 里再下发一次**（mpv 把 `deinterlace` 记进 watch_later，换片时会
  用该文件上次的值复位，全局开关必须盖回去）。`-- --deinterlace=no|yes|auto`
  优先于持久化值（本会话生效、不写回设置），见 `main.cpp` 的 passthrough 解析。
- 实测（合成的 25i TFF h264，1280×720；另一个 1080p59.94 逐行素材）：
  `no` → `deinterlace-active=no`、帧率 25（VFG×2→50）；`auto`/`yes` → active=yes、
  **deint 输出 50**（`vsr-status fps=50`，超分成本×2）、VFG 再×2→100；
  逐行素材强制 `yes` 也会跑 bwdif（软化细节，所以默认 no）。切换即时生效、
  `hwdec-current`/`pixelformat` 始终 `nvdec`/`cuda`（= bwdif_cuda，无
  `creating deinterlacer failed`）。个别切换会触发一次解码器重置（日志多一行
  `● Video` + `vsr: init`，~300 ms 级），多数切换不重建链。
- Toggle 与 `send_field` 的连带影响：反交错在链最前面 → VSR 按翻倍后的帧数计费；
  VFG 按 `k × 反交错后帧率` 判 `max-fps`（默认 60：50p 源 ×2 = 100 > 60 → 直通）。

### 桌面集成（2026-10-17）

- `scripts/install.sh` 装应用菜单入口：入口内容 = **`data/vsr-player.desktop.in`**（单一事实源，中英 Name/Comment；`@EXEC@` 由脚本替换为安装后绝对路径，不依赖 PATH），图标 = `data/vsr-player.svg` + `.png` → hicolor `scalable`/`512x512`，之后 best-effort 刷新 `update-desktop-database`/`gtk-update-icon-cache`/`kbuildsycoca6`。打包脚本把 `data/` 放进 tarball 根（`build_release.sh` staging）。
- `main.cpp` 里 `QGuiApplication::setDesktopFileName("vsr-player")`：Wayland app_id / X11 WM_CLASS 钉成与 `.desktop` 同名，否则任务栏图标与窗口分组会失配。
- ⚠️ **图标是占位**（用户 2026-10-17："后续做个像样的"）：正式图标只需替换 `data/vsr-player.svg`（矢量单一事实源）并跑 `scripts/render-icon.sh` 重生成 PNG。
- `Exec=… %f`（单文件/目录）：2026-10-18 起 `Options::parse` 接受多个位置参数（= 播放列表条目，见上），所以 `%U` 已经可用；当前**没有**改 `data/vsr-player.desktop.in`（改它等于让"文件管理器多选"变成播放列表，需要时再定）。

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
- **读回真实 OSD 文本**（无需截图/目检）：`--benchmark` 启动会调 `showOsd()` → OSD 文本被推给 mpv 的
  `osd-msg1`（普通模式只在 OSD 可见时推）→ `{"command":["get_property","osd-msg1"]}` 经 RPC 读回。
  2026-10-18 用它验证 OSD 标签 i18n（中文/英文标签一眼可见）
- 关键文件：`src/mpv/video/filter/`（vf_vsr.c/vf_rife.c/vf_hwup.c/vsr_proc.c/rife_proc.c/rife_trt.cpp/cuda_shared.h/yuv_to_rgba.*）；`src/client/`（main.cpp/MpvController/PlayerViewModel/Video.cpp/Options.cpp/RpcServer.cpp）
- 视觉问题数值化：`tests/verify_frames.sh`（baseline/test 两阶段 PSNR）——无 baseline 不改代码
- **gdb 调试**（C/C++ 崩溃/卡死）：`gdb -batch -ex run -ex bt <binary>`（崩溃看调用栈；卡死看线程）——SIGSEGV→bt 定位非法访问；SIGABRT→bt+寄存器看 assert 条件；卡死→bt 看所有线程在等什么。通用排查流程见 `~/.dsh/AGENTS.md` 调试方法论

## 相关文档

- `README.md`/`README_zh.md`（功能/构建/分发/CLI）、`docs/`（设计记录与参考索引）。调试守则/方法论在 `~/.dsh/AGENTS.md`（全局）
- **`docs/memory/`** — 历史调研/调试记录（源自 Claude Code 会话记忆，筛选保留 6 个知识类主题 + `README.md` 索引；过程性/过期记录已移除）。⚠️ 部分内容已过时（如 RIFE 4.25、闪回未解决），引用前对照代码，以本文件为准
