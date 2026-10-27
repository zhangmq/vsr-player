# AGENTS.md — vsr-player 项目记忆（DeepSeek Harness 版）

> 本文件由 DeepSeek Harness 转写：来源 = 开发机上的 Claude Code 会话记忆（已筛选：保留知识/经验类 6 个主题，移除过程性/过期记录；清洗隐私/密钥后拉入仓库 `docs/memory/`）+ 原 `CLAUDE.md`（已于 2026-08 删除，信息脱节），并已对照 master 代码（`7bae9ce`）修正其中的失效/冲突信息。
>
> **⚠ 当前在办任务（接手先读）**：`docs/handover-3d-conversion.md`
> —— 把 `test.iso` 的 6 个 title 离线转出来（2D 直接压 / 3D 转全宽 SBS），含阻塞点、工具位置、编码参数与已知坑。
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
8. **不做人类式工时估算，也不做"最小改动"式偷懒**（2026-10-27 用户要求）——你是按 token
   写代码的 agent，不是人：
   - **不说"半天/一天/约两小时"**，也不用"工作量"来给方案排序或劝退；
   - **不以"改动小/省事"为取舍标准**：不跳过需求里的部分、不留下半成品路径、不为了少改
     几个文件而把逻辑塞进不合适的位置。需要动几层就动几层（滤镜/客户端/UI/文档一起对齐），
     实现要对得起规格；
   - 方案只按**依赖顺序**拆成可执行步骤，每步给出**可验证判据**（能跑的命令 / RPC / 日志行），
     做完即报结果。

## 已修问题的两个易复发点（2026-10-26，用户报"像是修过的 bug 复现"）

### 1) 播放列表清空后画面停在最后一帧

- **根因不在 mpv**：清空（`playlist-clear`+`playlist-remove 0` 或逐个删到空）后
  mpv 确实 `idle=yes`、`core-idle=yes`，但**我们的渲染循环是"有新帧才画"**
  （`Video::requestRender` 判 `mpv_->update()>0`）：idle 之后不再有新帧，
  `rtImage` 里留着最后一帧，合成节点每帧照旧采样它 → 画面永远停在最后状态。
- **修法**：`Video::blank_` + 合成 shader 的 `blank` 标志（`CompositeParams.h`
  与 `composite.frag` 的 push constant 各加一个字段）。`PlayerViewModel` 在
  **列表非空 → 空**的跃迁和 `stop()` 上发 `playlistEmptied()` → `Video::setBlank(true)`
  → 请求一次重绘，shader 直接输出 `vec4(0)`；`MPV_EVENT_FILE_LOADED` 时复位。
  这样**任何一次重绘**（包括锁屏后解锁、切窗口）都是黑的，不依赖 mpv 清帧。
- ⚠ 不要改成"靠 mpv 自己清 rtImage"：`mpv_render_context_render` 在没有帧时
  是否清屏取决于 vo/gpu 内部状态，实测清空列表后窗口仍是最后一帧。

### 2) 重启后点播放无反应（概率性）

- **根因**：`playlist` 观察器在 `files.isEmpty()` 时无条件清
  `lastPath_`/`idlePlayIndex_`（那是为"移除后别复活"加的）。但**启动 idle**
  时 `restorePlaylistIdle()` 先把重播目标设好，随后观察器投递的**初始空快照**
  会把它抹掉 → `play()` 的 `!fileLoaded_` 分支无目标、直接 return。事件与启动
  代码的先后不定 → 表现为**概率性**（用户："似乎存在一定几率触发"）。
- **修法**：只在**非空 → 空**的跃迁上清（`PlayerViewModel::playlistCount_`
  记录上次条目数）；初始空快照不再算"用户清空了列表"。`play()` 的早退分支
  会记 `play: nothing to resume (...)`，下次复现直接看日志。
- 验证：连续 3 次"重启（idle，count=1/pos=-1）→ 点播放"→ 每次都 `pos=0`、
  `path` 正确、`video-params/w=1920`。

### 3) stop 也走同一条置黑路径

`stop()`（`stop keep-playlist`，列表保留、媒体卸载）以前同样停在最后一帧，
现在也 `emit playlistEmptied()`；重播时 `FILE_LOADED` 复位。实测
`stop → video: blank` / `play → video: content`，并回到原条目。

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
- **播放列表条目名（蓝光 title）= 交给 mpv，不在客户端做名字表（2026-10-27）**：
  - 症状：ISO 展开的 title 全显示成 iso 文件名；**关掉再以无参数启动**（idle 恢复列表）
    之后又变回 iso 文件名。
  - 根因（两段）：① `PlaylistModel` 只按路径推导显示名（`bd://mpls/N//…` 的 basename
    就是 ISO 名）；② 启动恢复是逐条 `loadfile bd://mpls/N//…`，**这些条目没有 title**
    （title 只在 `parse_bluray` 展开 ISO 时才有），于是名字又丢了。
  - 结论：**名字的唯一来源是 mpv 的 playlist 条目 `title`**
    （`player/command.c` 的 `{"title", SUB_PROP_STR(e->title)}`），客户端只负责显示。
    曾经试过在客户端维护"路径→名字"表和 `bdisc::labelFor` 注册表，被用户否掉
    （"正确的方法不是从 demux 那块入手吗"）——确实是 hack：既重复了 mpv 的能力，
    又要在"空 title 不覆盖旧名字"上打补丁。
  - **落地做法**：播放列表持久化成 **m3u8 快照**（`~/.local/share/vsr-player/last-playlist.m3u8`）：
    ```
    #EXTM3U
    #VSR-CURRENT:<当前索引>          ← 自定义注释，mpv 忽略
    #EXTINF:-1,Title 6 · 1:18:30    ← 条目名（mpv 给的 title）
    bd://mpls/00003//home/…/x.iso
    ```
    恢复时用 mpv 原生 **`loadlist <快照> [append]`** → `demux_playlist` 的 m3u 解析器
    把 `#EXTINF` 变成 `e->title`（`parse_m3u`，与 ISO 展开同一套机制）；
    条目名再经 `PlaylistModel::setSnapshot(paths, current, titles)` 显示。
    `restoredFiles()` 直接解析这份快照（缺文件时回退旧 `playlist`/`playlistCurrent`
    settings 键，兼容老安装）。
  - 结果：`bdisc::labelFor/registerLabel/urlFor` 与 `BlurayTitle::label` 已**全部删除**；
    `PlaylistModel::displayName()` 回到"只用 basename"，名字一律来自 mpv。
  - 验证钩子：RPC `get-playlist` 打印 mpv 侧 `title/filename`（带 `*`=current、`P`=playing）
    与模型侧 `model.display`。实测：ISO 会话与无参数重启都显示
    `Title 1 · 0:00:00 … Title 6 · 1:18:30`；普通视频列表仍显示文件名，索引也随快照恢复。
  - ⚠ **恢复后的 bd:// 条目一开始点不动（2026-10-27 修）**：`bd` stream 的
    `stream_origin` 是 **UNSAFE**，mpv 的 origin 检查只放行 DIRECT 来源；m3u 快照
    是本地文件（FS 来源）→ 里面的 `bd://` 条目被拒。而 `stream_create()` 的循环把
    UNSAFE 当 NO_MATCH 继续，最终**报成 "No protocol handler found"**（很容易误判成
    "URL 存错了"）。ISO 展开那条路径没事，是因为 `parse_bluray` 分支已把条目提升为
    DIRECT（我们自己的 patch）。修法（`src/mpv/demux/demux_playlist.c`）：本地
    播放列表（`demuxer->stream_origin == STREAM_ORIGIN_FS`）里以 `bd:`/`bluray:`
    开头的条目逐条提升为 `STREAM_ORIGIN_DIRECT`；**远程列表仍按 mpv 原始策略拒绝**。
    验证：重启→列表有 title→点任一 title 正常播放（日志 `Playing: bd://…` 后直接出
    音频/视频流，无协议错误）。
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

### ⚠ 运行期选项"重建继承"（2026-10-27 修：关掉旋转后重播仍是旋转的）

**现象**（用户报）：播放中把旋转切到 90° 再关回 Off，之后每次播放**开始瞬间画面仍是旋转的**。

**根因**：链串（`vfOption()`）是**加载时**的快照，而运行期状态走 `vf-command`。二者不一致时：

1. mpv 重建用户滤镜链（解码器重配/换片/格式变化）→ 用**链串旧值**建实例（angle=90）；
2. 客户端在 reconfig 后又把当前值推回去（angle 0）→ 输出尺寸 1080×3840 ↔ 3840×1080 来回变；
3. 尺寸变化又触发重配 → 回到 1 —— **无限重建 + 尺寸翻转**（实测日志刷出几十轮
   `rot: 90 deg … -> angle 90 -> 0`，每次 `kernels compiled` 都是新实例）。

**修法**：滤镜实例重建时**继承"最后一次 vf-command 的状态"**（进程内 static，
`vf_rot.c` 的 `g_last_angle`、`vf_eye.c` 的 `g_last_mode/layout/half`），而不是回到链串值。
这样重建后立刻就是当前状态，尺寸不再翻转 → 循环消失、启动瞬间也不再闪旋转。

**验证**：配置里预置 `rotMode=90` 启动 → 播放中 `set-rotate 0` → `stop`/`play`：
重播瞬间尺寸就是 3840×1080；`rot: kernels compiled` 计数从几十次降到 **1** 次。

⚠ 这套 static 只对**会改变输出尺寸**的选项（rot / eye）是必需的（不改尺寸的选项
链串与运行期不一致最多影响一帧画面，不会形成重建循环）。`vf_vsr` 的 scale 实测不改
链串、也不触发重建循环（`vsr: init` 2 次，`vsr: reset` 0 次）。

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

### 3D 下的 VSR auto 倍率（2026-10-26，**按每眼算**）

**结论**：`scale=auto` 在 3D 下必须按"**单眼视口 vs 单眼源像素**"算，不能用整帧 vs
整块 framebuffer（那是 2D 的算法）。

```
eyeW = outW/2, eyeH = outH          （输出 framebuffer 恒为 SBS）
imgW/imgH（单眼源像素）:
  SBS 全: (srcW/2, srcH)      SBS 半: (srcW, srcH)
  TAB 全: (srcW, srcH/2)      TAB 半: (srcW, srcH)
need = min(eyeW/imgW, eyeH/imgH)    → 再按 SDK 支持集取档（4/3,1.5,2,3,4；≤1 → 1×）
```

- **SBS 全与 2D 公式代数等价**（`min((outW/2)/(W/2), outH/H) = min(outW/W, outH/H)`）
  → 历史上只用 SBS 全测试，所以一直没暴露。
- **TAB 全会偏软**：源每眼只取半高，2D 公式按整高比 → 实测 960×720 TAB 在
  3840×1080 上应 2.0×，旧公式只给 1.5×；1440×1080 TAB 应 1.33×，旧给 1.0×。
- **半幅会偏大**（源已沿 split 轴拉伸到整帧）：1920×360 SBS 半 应 1×，旧公式 2×。

**实现**：`vf_vsr` 两个选项 `stereo-layout`(-1|0|1) / `stereo-half`(0|1) 进**链串**
（所以链重建后自动恢复），运行期 `vf-command vsr stereo <sbs|tab> <full|half>|off`
热切（不重建链）。客户端由 `PlayerViewModel::setStereoMap()` 在
`StereoController::stateChanged`（含 J/K/L）时推。

⚠ **链重建会抹掉运行期 vf-command**：mpv 在解码器软硬切换/格式变化时用
**加载时的 vf 选项串**重建用户滤镜链（不只是 3D——rot/truehdr/vfg 的运行期值同样
受影响）。实测 3D 倍率在软硬解码切换后回到 2D 公式。修法 = 客户端挂
`MPV_EVENT_VIDEO_RECONFIG` → `replayStereoMap()` 补推（链串里也带值，双保险）。

**实测（本机 3840×1080 framebuffer，30fps 源 + VFG×2 → 60fps；目标 1080p@60）**：

| 场景 | 倍率 | VSR cost | 实测 fps | 丢帧 |
|---|---|---|---|---|
| 3840×1080 SBS 全（1080p 每眼） | 1.00 | passthrough | 60.0 | 0 |
| 3840×1080 SBS 半 | 1.00 | passthrough | 60.0 | 0 |
| 1920×1080 SBS 全 | 1.00 | passthrough | — | 0 |
| 1920×2160 TAB 全 | 1.00 | passthrough | 60.0 | 0 |
| 1280×720 SBS 全 | 1.50 | 3.4 ms | **60.00** | 0 |
| 960×720 TAB 全 | 2.00 | 2.3 ms | **60.00** | 0 |
| 640×360 SBS 全 | 3.00 | 1.0 ms | **60.00** | 0 |

GPU 占用：1080p 每眼 SBS（VSR 直通）+ VFG×2 @60fps = **13%**（RTX 5060 Ti）。
即 1080p@60 在 3D 下有大把余量；VSR 真正活跃的只有低于 1080p/眼 的 3D 片源，
最贵一档（1.5×）也只有 3.4 ms/源帧（×30 = 约 10% GPU）。

⚠ 测试钩子（无眼镜时也能测 3D 路径）：`set-stereo-map off|<sbs|tab> <full|half>`
（走客户端同一入口、会被重放）；另有通用 `vf-command <filter> <cmd> [arg]`。
测 3D 的 framebuffer 尺寸时用 `resize 1920 540`（DP-3 是 dpr 2 → framebuffer 3840×1080）。

### 3D 输出（OSD + 视频映射，2026-10-25，分支 feat/stereo-osd-prototype）

- 入口/流程：右键菜单 3D 三态 **Off / 3D片源 / 2D转3D**（运行期、不持久化，默认 Off）。
  3D片源 → 选择器（QScreen `model` + `name` + **物理分辨率**；**不假设 1080×2/
  不按宽高比猜眼镜**；屏幕记忆只做默认选中，不自动切屏/启动）→ Start。
  2D转3D 本期只弹提示，不启动。
- ⚠ Wayland 下 `QScreen::geometry` 是**逻辑**尺寸（VITURE 报 2844×800，物理
  3840×1080）；选择器通过 `kscreen-doctor -j` 的 `currentModeId` → mode `size`
  取物理分辨率（KDE/KWin 专属；找不到工具时退回 QScreen 尺寸）。`QScreen::model`
  （VITURE/DELL/LG）用于显示名称。
- 菜单/选择器关闭顺序：`ContextMenu` 二级项**先发 subSig 再 close**——
  close 会拆 popup 的临时 grab/内容，之后 `root[subSig]` 的动态调用可能不生效
  （2026-10-25 实测 3D 二级项点击无反应）；主 QML 在 `onStereoSelected` 里
  **先 `contextMenu.close()`，再 `Qt.callLater` 打开选择器**，不能依赖
  “打开另一个 popup 自动关旧的”。`StereoPicker` 的 Start 仍先 close 再
  `stereo.start`。选择器打开时不再同步跑 `kscreen-doctor`（屏幕列表由
  screenAdded/Removed/geometryChanged 维护），避免点菜单后卡顿。
- ⚠ **Popup 生命周期（2026-10-25，不是 Qt 屏 bug）**：`ContextMenu`/`StereoPicker`
  都是默认的 **`Popup.Item`**，内容项被 reparent 到窗口 overlay，不是独立窗口；
  打开另一个 popup **不会**自动关前一个，`uiLayer.visible=false` 也**不会**隐藏
  overlay 里的 popup。所以打开选择器前必须显式关菜单；`stereo.active=true` 时
  主 QML 关闭主窗口所有 popup（context/playlist/picker 等），否则它们会跟着
  主窗口一起移到眼镜屏。Qt 文档/源码（`qquickpopup.cpp` 的
  `adjustPopupItemParentAndWindow`、`qquickpopupwindow.cpp`）与社区报告
  （QTBUG-67684、QTBUG-35261）都是这个语义，不是 QScreen/合成器放错屏。
- **Context menu 仅 UI 窗口可用**（2026-10-25 用户要求）：`MouseArea` 与
  `ContextMenu.enabled = controlMode || !stereo.active` —— 2D 用 main 窗口，
  3D 用 control 窗口（桌面主 UI）；presentation 窗口禁用右键/菜单。
- **Context menu 及时关闭**：选 3D 片源时 `onStereoSelected` 先
  `contextMenu.close()` 再 `Qt.callLater` 开选择器；`stereo.active` 变 true、
  `uiLayer.visible=false`（自动隐藏/进 3D）、`window.active=false`、
  `window.visible=false` 都显式 `contextMenu.close()` + `stereoPicker.close()`；
  打开其他 popup 仍走 `uiLayer.closeOtherPopups()`。
- 3D active 期间 `Escape` Shortcut 禁用（主窗口被合成器聚焦时 Esc 退全屏会切回
  焦点屏）；退出用桌面 3D → Off。
- **主 UI 始终在桌面**：KWin 把新窗口放到当前活动输出——用户鼠标在眼镜屏时
  主 UI/右键菜单会开到眼镜上。`main.cpp` 在 `view.show()` 后把主窗口移到
  `QGuiApplication::primaryScreen()`；`StereoController` 选 control 屏时若
  主 UI 原来就在目标（眼镜）屏，改用 primary/其他非目标屏。control 窗口用
  `show() → setScreen(controlScreen) → setGeometry()`（Wayland 初始放置看
  active output，show 前 setScreen 不生效）。`main.cpp` 另建一个
  `QQuickView controlView`，加载同一份
  `main.qml` 但 `controlMode=true` → `Video` Loader 不创建，只有 UI。2D 时它隐藏、
  主窗口（Video+UI）在桌面；3D 时主窗口去眼镜屏做 presentation，controlView 在主窗口
  原来的屏幕/几何上显示，主 UI 不消失。3D 快捷键只在 control 窗口启用（两个窗口都载入
  main.qml，避免 `Qt.ApplicationShortcut` 重复触发）。
- **⚠ 主窗口全屏时投不到眼镜屏（2026-10-26 用户报，已修）**：KWin 把**全屏窗口
  钉在它所在的输出上** —— 对全屏窗口调 `setScreen()` 被忽略，`showFullScreen()`
  又是 no-op（本来就是全屏），于是窗口留在原屏；`get-stereo` 会显示
  `out=3840x2160`（原屏）而不是眼镜的 3840×1080。**修法**：换屏前先 `hide()`
  （`showNormal()` 不够），再 `setFlags` → `setScreen` → `showFullScreen`；
  `stop()` 恢复正常屏幕时同理（否则退出 3D 后画面留在眼镜屏）。两处都要，
  只修 start 会出现"进得去、出不来"。实测（DP-3 3840×2160 全屏 ↔ DP-1 眼镜
  3840×1080）：`全屏 3840×2160 → 3D 3839×1080 → 退出 3840×2160`；窗口态
  `2560×1440 → 3839×1080 → 2560×1440`，可重复进出。
  ⚠ 日志只记屏幕名（`stereo: presentation window -> DP-1 (was fullscreen)`）：
  窗口 `geometry()` 在 `showFullScreen()` 之后还没稳定，记出来是旧值，会误判。
- **presentation** = 主窗口（Video/VO）：`setScreen(眼镜屏)` + `showFullScreen`，加
  `FramelessWindowHint | WindowStaysOnTopHint | WindowDoesNotAcceptFocus`，光标 Blank；
  主窗口 uiLayer 在 3D 时隐藏；3D 快捷键帮助面板是 presentation 内的 `StereoHotkeyHelp.qml` 覆盖层
  （Loader，z=100），不是独立窗口。`H` 只切覆盖层；`J/K/L` 改布局/眼序/全半。
  实测 KWin 下满屏置顶覆盖了任务栏/面板，无其他可见内容。停止时恢复主窗口 flags/
  光标/屏幕/几何，隐藏 controlView。
- **屏拆分在 VO/合成阶段**：`vf_*`（VSR/VFG/TrueHDR）链路不动，滤镜仍处理完整
  源帧；`CompositePipeline` 用 push constants 把输出窗口左右半映射到左右眼。
  实测 VSR 仍 active（scale 随 presentation 窗口自动变 1.5），VFG 的
  passthrough 是既有 `max-fps` 判定，不是 3D 改动。
- **映射语义**（用户 2026-10-25 定）：SBS=左右，TAB=上下；`L` 按 split 轴切
  **全/半**，全/半是**源眼输出宽高**的区别，不是 box：
  - **全**：源眼像素尺寸原样（SBS = srcW/2 × srcH；TAB = srcW × srcH/2），
    **不拉伸**；
  - **半**：源眼沿 split 轴拉成**源整帧像素尺寸**（srcW × srcH）：SBS 横向 2×、
    TAB 纵向 2×；
  - **最后统一按眼窗口 contain 等比缩放**：`scale = min(eyeW/imgW, eyeH/imgH)`，
    可放大/缩小、不拉伸，居中，余量留黑边。
  - 例 1920×1080 SBS：全 960×1080 → 960×1080，半 1920×1080 → 1920×1080；
    例 960×720 SBS：全 480×720 → **720×1080**，半 960×720 → **1440×1080**
    （眼视口 1920×1080）。
  目标屏按实际尺寸；输出 framebuffer 恒 SBS，因此**眼视口 aspect = outAspect/2**
  （曾误用 outAspect 导致画面窄一半，已修）。`StereoCompositeParams` 带
  `srcW/srcH/outW/outH`，全/半按源像素算（不是按眼视口缩放）。源帧在 rtImage 里是 mpv letterbox 后的
  内容，先算 contentRect 再取样。无红绿/anaglyph。
- **分屏必须整数化（2026-10-26，用户报"左右眼偏差"）**：Wayland 分数缩放下
  framebuffer 宽会是**奇数**（VITURE：逻辑 2844×800、scale 1.35 → 物理 3840×1080，
  但 `win->width()*dpr` = **3839**）。按 `uv.x < 0.5` 分屏时 eyeW = 1919.5：
  - 两眼采样网格差**半个像素** → 整幅画面恒定水平视差（等于给所有内容加了一个
    会聚偏移）；实测两眼的差异在±1 像素平移测试里最小值落在 0 与 1 之间；
  - 修法两层：① 渲染目标/合成视口宽度**向上取偶**
    （`Video::compositeWidth()` 3839 → 3840，多出的 1 列落在窗口外被裁掉），
    于是 `splitX = outW/2 = 1920`、眼 1920×1080、`scale = 1.0` → **1:1 像素、
    无重采样、无黑边**；② `composite.frag` 分屏按**整数像素**判（
    `px = uv.x*fbW; eye = px < splitX`，两眼都用 `splitX` 作眼宽、超界夹取边缘），
    不再有 `outW/2.0` 这种小数参与几何。
  - 验收（自造片：左右两半**逐位相同**的静态 SBS，`grab-window` 后逐像素比）：
    修前/修后 = 眼内 mean|Δ| 0.39 → **0.0003**、p99 14 → **0**（残余只剩画面
    最外侧 1–2 列 = mpv 对源帧边框的 clamp）；DP-3（偶数 3840 宽）对照组
    同样是 p99 = 0。满屏判据也从每眼 1919×1079（底部 1 行黑）变成 1919/1920×**1080**。
- **⚠ 暂停时切 3D 会只剩半屏（2026-10-26 修，用户"3840×1080 不能满屏"的真凶之一）**：
  rt 重建走"尺寸连续 `RT_STABLE_FRAMES`(10) 帧未变才重建"的节流，靠 updatePaintNode
  被反复调用推进；**暂停时没有新帧 → updatePaintNode 不再被调 → 计数器停在 1**，
  rt 永远停在旧尺寸（如 2560×1440）。此时 mpv 仍把 3840×1080 塞进 16:9 的旧 rt
  （画面只占中间 50% 高），而 composite 已按新窗口取样 → 看起来就是"不能满屏"。
  修法：① 尺寸未追平时在 updatePaintNode 末尾自我驱动 `update()`（到阈值即停，
  不会无界循环）；② 重建成功后 `renderRequested_ = true` **强制本帧重画**——新建的
  rt 是空图，暂停时 mpv 不会自己再画一次（只修①会得到"rt 对了但画面全黑"）。
  实测：暂停 + seek 后切 3D，日志出现 `render target 3840x1080` 且画面满屏。
- **3D 映射参数有 INFO 日志**：`stereo map: out=… splitX=… src=… layout=… half=…
  swap=… -> eye=… content=…`（参数变化时一行，`Video::stereoParams`）。
  用户报"不满屏/两眼错位"时先看这行：`eye` 是每眼视口、`content` 是实际贴图尺寸
  （= 眼视口 = 满屏；比眼视口小 = 有黑边，差值就是黑边）。
- OSD：默认显示；`Tab` 显隐；`J` 布局、`K` 交换眼、`L` 全/半；标题/键名 Bold、
  正文 DemiBold；左右面板 ±12 logical px 交叉视差（OSD 浮在画面前，独立于视频眼序）。
- **3D 快捷键帮助面板（HotKeys，2026-10-25/26）**：**两列、一键一行、无分组名**。
  左列 3D：H / J / K / L；右列播放器：Space / ← / → / Shift+← / Shift+→ /
  ↑ / ↓ / Ctrl+← / Ctrl+→（方向键、Shift+方向键、音量上下、上/下一个都各占一行）。
  `HotkeyRow` 的键名列**统一 100px**，状态/说明从同一 x 起（所有行左对齐）；
  3D 状态**不用按钮**，纯文本 + 颜色区分（选中 `#ffcc00`，未选 `#e0e0e0`），
  避免按钮宽度破坏对齐；切换后该行淡黄闪 500ms。OSD 全部文字只用
  **一个字号 = 18**（标题/键名/状态/说明同字号；标题也不加粗/不放大），
  不再有 20/24/32 等多套；行高 32。
  左列 `H: Toggle HotKeys Help`、`J: 左右并排 / 上下并列`、`K: Normal Eyes / Swapped Eyes`、
  `L: Full / Half`；右列 `←`/`→`（±5s）、`Shift+←`/`Shift+→`（±10s）、
  `↑`/`↓`（音量 ±）、`Ctrl+←: Previous`、`Ctrl+→: Next`（中文“上一个/下一个”）。
  OSD panel 宽高按内容（`contentWidth` 在 Repeater delegate 内，不能放 root），
  左下/右眼两份 + 交叉视差不变。
- ⚠ **3D 快捷键帮助面板的 i18n 输入**：`StereoHotkeyHelp.qml` 不在 client 根目录的 `ui/*.qml`
  glob 里，`scripts/update-translations.sh` 已显式加入；否则 OSD 全部回退
  英文（2026-10-25 修）。当前 zh_CN OSD：热键/左右并排/上下/正常眼序/
  交换眼序/全/半/播放器/播放暂停/快退快进/音量。
- ⚠ **3D 热键统一装在 `QGuiApplication` 上**（2026-10-25）：装在两个
  `QQuickView` 上时，3D 焦点可能在 presentation/control 任一窗口，事件到不了
  过滤器。`StereoKeyFilter` 现在 `qApp->installEventFilter()`，按
  `event->key()` 拦截：H/J/K/L（切换）+ Space（播放暂停）+ ←/→（±5s）+
  Shift+←/→（±10s）+ ↑/↓（音量）+ Ctrl+←/→（上一个/下一个）。大小写无关、
  允许 Shift；Alt/Meta 不拦；Ctrl 只处理 Ctrl+←/→，其它 Ctrl 组合放行。
  切换类忽略 auto-repeat，方向/音量允许连发。
  QML 里 3D 的 J/K/L 已删除，Space/方向/音量在 3D 时禁用
  （`&& !stereo.active`），只留 2D 路径；C++ 负责 3D，避免重复触发。
  **OSD 帮助键已从 Tab 改为 H**（用户 2026-10-26 定）：所有 Tab/Backtab 相关代码
  已全部回退/删除（app filter、QShortcut、QML Shortcut、QQuickView::event 特判、
  send-key RPC），`WindowDoesNotAcceptFocus` 与 presentation 的 flags 恢复原样。
  H 是普通键，走 `StereoKeyFilter` + `QShortcut` + QML `Shortcut` 三条普通路径。
  注意：2D 主窗口的 QML `Shortcut` 本来就能触发 Tab（有焦点的主窗口），
  3D 失效是当时该 Shortcut 被 `!stereo.active` 禁用后，C++ 路径又受窗口焦点影响，
  不是“Tab 不能由 QML 处理”。
- **上一个/下一个 = 全局 Ctrl+← / Ctrl+→**：2D 走 QML `Shortcut`
  （`enabled && !stereo.active`），3D 走 C++ 过滤器；`Ctrl+L` 是 URL 输入，
  与 Ctrl+方向键不冲突，项目内无重复绑定。
- **3D 进行中提示（2026-10-26）**：桌面 control 窗口正中显示 **MDI `glasses`
  U+F02AA** 大图标（`pixelSize: 96`）+ `qsTr("请戴上眼镜观看")`（18px，`#e0e0e0`），
  显示条件 `controlMode && stereo.active`。视频在眼镜屏上，桌面这块本来就是空的
  （controlMode 不创建 Video）。⚠ 图标码点必须写**字面字符**：`\uF02AA` 会被
  QML 解析成 `\uF02A` + "A"（本项目踩过两次；我第一次查 cheatsheet 也把 U+F02A8
  = glass-tulip 当成了 glasses，截图里是个高脚杯才发现——正确是 U+F02AA）。
- **3D 下 Tab 切管线状态 OSD（2026-10-26 用户要求）**：`StereoKeyFilter` 在 3D 时
  吃掉 `Key_Tab`/`Key_Backtab` → `viewModel.toggleOsd()`，2D 仍走 QML `Shortcut`。
  ⚠ **3D 下 mpv 的 OSD 必须关掉**：osd-msg1 是画进视频帧的，合成器按左/右半幅切开
  → 每行只剩半个字，完全不可读。所以 `osdPoll()` 里 3D 时强制 `osd-level=0`，文本
  另存 `osdText`（互斥量保护，事件线程 emit `osdTextChanged`），由 presentation
  窗口的 QML 覆盖层 `pipelineOsd` 画：左右眼各一份、**零视差**（落在屏幕平面）、
  左上角 24px、`font.family: "monospace"`（osdTextString 按显示宽度补空格对齐，
  等宽才不歪）、带 `Text.Outline` 保证在亮画面上可读。`osdVisible` 是单一状态，
  两种渲染器共用（新增 `Q_PROPERTY(bool osdVisible)` + `Q_PROPERTY(QString osdText)`）。
  测试钩子 `toggle-osd`（不注入按键）。
- **3D 二级面板分组（2026-10-26）**：`submenu` 支持 `{header: "…"}` 分组标题与
  `{group: 1, value: …}` 分组 id；点击时 `root[it.subSig](value, group)`，
  勾选态用 `it.current(group)` 按组求值（`signal stereoSelected(int value, int group)`）。
  3D 面板现在是两组：**输出设备**（`XR 眼镜`，值为 `stereo.outputType`，默认 0）
  + **显示方式**（不使用/3D片源/2D转3D，值为 `stereo.mode`）。`outputType` 目前
  不影响渲染路径，是前向占位（将来接 3D 电视需 SBS-half 拉伸几何、VR 头显需 OpenXR）。
  ⚠ **分组分隔线只在两组之间**（用户 2026-10-26 定）：第一个 header 上方不画线
  （`visible: isHeader && index > 0`），否则面板顶部会多一条贴边线。
  ⚠ **面板不能超出主菜单高度**：popup 高度 = 主菜单高度，二级面板是自绘子项，
  超出会被裁掉 → `subY` 按 `mainPanelH - subPanelH` 做底部钳制并整体上移；
  `subPanelH` 按实际行高累加（header 行矮 4px）。
- **进度提示（2026-10-26）**：`ui/StereoProgress.qml`，与音量提示同一套机制
  （底部居中面板 300×56、1.5s 自动隐藏、3D 左右眼各一份 ±12 logical px 视差、
  `#cc111111` 面板 + `#44ffffff` 轨道 + `#e0e0e0` 填充），显示"当前 / 总时长"+进度条。
  **只在 3D 显示**（2D 桌面 UI 本身有进度条）；触发源 = `viewModel.seekNudged()`
  —— 只有 `seekRelative()`（快捷键/方向键）发，拖进度条走 `seekAbsolute()` 不发。
- **音量/进度提示的触发源必须区分 UI 与快捷键（2026-10-26 用户要求）**：
  `PlayerViewModel::nudgeVolume(delta)` 供快捷键用（↑/↓ 的 QML Shortcut 与 C++
  `StereoKeyFilter` 都走它），它发 `volumeNudged()`；滑杆走 `setVolume()` 不发。
  `StereoVolume` 只听 `volumeNudged` → 2D 下**拖滑杆不弹**、按 ↑/↓ 弹（用户批准）。
  实测（像素差，基准=静置帧）：2D 快捷键音量 8136 px / 2D 快捷键快进 0 px /
  2D 滑杆改音量 0 px；3D 快捷键快进 32270 px、音量 31078 px，1.8s 后均归 0；
  两眼面板中心 +16 / −15 物理像素（= 12 logical × dpr 1.35）。
  测试钩子：`nudge-volume <±delta>`（模拟快捷键，与 `set_property volume` 区分）。
- **音量提示（2026-10-26）**：独立的 `ui/StereoVolume.qml`，**不是** 3D 快捷键
  帮助面板内容，与 `helpVisible` 完全无关。2D 一份；3D 左右眼各一份（±12 视差）。
  音量变化时显示 1.5s，系统 OSD 风格（扬声器图标 + `#44ffffff`/`#e0e0e0` 横条 +
  百分比）；2D/3D 都显示，`controlMode` 不显示。绑定 `viewModel.volumeChanged`。
- **3D 映射变化必须重绘当前帧**：`StereoController::stateChanged` 连到
  `Video::update()`；否则暂停时切 L（全/半）只改帮助面板状态，画面要等下一帧。
- **窗口关闭语义**：`StereoView`（presentation/3D 窗口）在 3D 时收到 close
  → `ignore()` + `stereo.stop()`，3D 菜单随 `mode_=0` 回退“不使用”，不退出；
  2D 时正常关闭 → 退出。`ControlView`（桌面主窗口）关闭 → `quit()`。
- **文案**：“左右并排”不变；“上下”统一为“上下并列”（zh_CN OSD 译文）。
- 不做“切回 2D 自动结束 3D”（目标不一定是 1080×2，无法可靠判断）；只有目标屏被
  拔掉时 stop。`Esc` 不参与 3D。
- 测试钩子（RPC）：`stereo-start <1> <screen-index>`、`stereo-stop`、
  `stereo-help <0|1>`、`stereo-cycle layout|swap|half`、`stereo-picker <1>`、
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

### 蓝光 ISO / BDMV（2026-10-26，按"文件夹"展开）

**容器语义（2026-10-26 用户要求，"像文件夹 / m3u 那样"）**：**扩展了 mpv**——
`.iso` 由 `demux_playlist` 展开成 title 条目，客户端不再自己造 URL。三个 overlay 文件：
- `src/mpv/demux/bluray_image.h`（新增）：`mp_is_bluray_image()`——本地路径 +
  扩展名 `.iso` + `bd_open()` 能枚举出 title 才算（只读元数据，毫秒级）。
- `src/mpv/demux/demux_playlist.c`：新增 `parse_bluray()`（挂进 `playlist_formats[]`
  首位），生成 `bd://mpls/<%05u>//<绝对路径去掉前导 />` 条目并设 `title`
  （`Title N · H:MM:SS`）。
- `src/mpv/demux/demux_lavf.c`：入口处对蓝光镜像 **主动让路**（`return -1`）。
  ⚠ 必须这么做：demuxer 列表里 lavf 排在 playlist **之前**，而 lavf 会"认领"任何文件
  （把 ISO 当成无流的 mpeg），不拦就永远走不到 playlist。
- `src/mpv/options/options.c`：`playlist_exts` 加 **`iso`**。
  ⚠ 不加的话"打开一个**只装了 .iso 的文件夹**"会得到 **0 条目**：目录展开
  （`parse_dir` → `test_path`）只按扩展名表挑选，而 `iso` 原本既不在
  video/audio/image、也不在 archive/playlist 表里 → 目录当成普通文件打开、
  报 `Failed to recognize file format`，播放列表里只剩一个点不动的目录条目
  （2026-10-26 用户实测报的正是这个）。
- ⚠ **stream origin 坑（最花时间的一处）**：`bd://` 的 `stream_info_bluray.stream_origin`
  是 `STREAM_ORIGIN_UNSAFE`（设备路径可指向任意文件），而 `check_origin()` 只允许
  DIRECT/UNSAFE 来源打开它；demuxer 展开出来的条目默认是 **FS** 来源 → 被静默拒绝，
  报 `No protocol handler found to open URL bd://...`（错误文案完全不提 origin）。
  修法：`parse_bluray` 成功后把这些条目提升为 `STREAM_ORIGIN_DIRECT`（镜像已本地打开
  并枚举成功，条目只是同一张盘里的 title，不是权限放宽）。
  对照：客户端用 `loadfile bd://…` 手塞条目时是 DIRECT → 一直能用，所以这个坑只在
  "让 mpv 自己展开"之后才暴露。

**交互（用户 2026-10-26 定，含一次修正）**：ISO 与普通文件**同等对待**——
展开成多个 title 条目后**按播放列表顺序播放**（首条播放、其余 append 排队），
命令行 / 打开文件 / 打开文件夹 / 拖入 dropzone 四条入口一致。
（最初实现是"列出来但不自动播放"，用户否掉："符合一般播放习惯，应该按顺序播放"。）
⚠ 蓝光 3D 盘的第 1 个 title 常常是 0:00 的 logo/警告片，按顺序播放会瞬间跳过它
再进第 2 个——这是"按顺序"的正常结果，不是 bug。

**实现**：
- `src/client/BlurayDisc.{h,cpp}`：进程内链 **libbluray**（meson 里
  `dependency('libbluray', version: '>= 1.0.0')`；standard 用系统的、full 已捆
  `libbluray.so.4`）。`bdisc::enumerate()` 只读 mpls/clpi 元数据（**不解码**），
  返回 `{index, mpls, durationMs, chapterCount, clipCount, label}`。
  ⚠ 命名空间不能叫 `bluray`——libbluray 头里有 `struct bluray`，同名会
  "redeclared as different kind of entity"（实测）。
- `PlayerViewModel::openFiles`：ISO **只把路径原样交给 mpv**（mpv 自己展开，见上），
  顺带枚举一次只为**登记友好名**（`bdisc::registerLabel`，注册 `%05u` 与不补零两种
  写法）；普通文件原样混排，首条 `loadfile` 播放、其余 `append`，`mode == 1` 全部
  append。
- **UI 入口必须能选到 ISO**：`FileDialog` 的媒体过滤器原本只有
  `*.mp4 *.mkv …`，`.iso` 只能靠"All files (*)"才看得见 → 等于没有 UI 途径。
  现在媒体过滤器含 `*.iso *.m2ts *.mts`，并单列 `Blu-ray images (*.iso)`。
  ⚠ 用 **mpls 编号**而不是 title index：libbluray 的 title 索引随
  `TITLES_*` 过滤条件变化，mpls 是盘上稳定标识。
  **设备编进 URL（2026-10-26，解决了多盘混排）**：条目 URL 是
  `bd://mpls/<n>//<绝对路径去掉前导 />`，即
  `bd://mpls/3//home/zmq/视频/test.iso`。每条自带设备 → 同一播放列表里放**多张
  不同 ISO** 也各自正确；URL 就是 mpv playlist 的 `filename` → **持久化免费成立**
  （重启后条目自带设备，不再依赖全局设置）；`watch_later` 续播同理能定位回同一张盘。
  ⚠ 两条硬约束（都实测过）：
  ① **必须双斜杠**：`bd://mpls/<n>/<device>` 在第一个 `/` 处切分设备串，单斜杠会把
     绝对路径的前导 `/` 吃掉（`/home/…` → `home/…`)；写 `///home/…` 又多一个斜杠，
     所以生成时把路径自己的前导 `/` 去掉、固定用 `//` 作分隔。
  ② **必须绝对路径**：相对路径会被解析成相对 root（`test.iso` → `/test.iso`）。
  路径含中文/空格都验证可用（不需要 URL 编码）。
  全局 `--bluray-device` 仍会设置一次作兜底（老持久化里的裸 `bd://mpls/N` 条目靠它）。
- 播放列表面板显示名：`bdisc::labelFor("bd://mpls/5")` → `Title 6 · 01:18:30`
  （进程级 QHash 在枚举时登记；未知 URL 回退 basename）。
- **持久化**：`bluray-device` 是全局选项，重启后 bd:// 条目会打不开
  （mpv 报 "No protocol handler found"）。所以把 ISO 路径存 `blurayDevice`
  （设置写入后**立即 `settings_.sync()`**——被 SIGTERM 杀掉时不落盘，实测），
  `restorePlaylist*` 在装载前先设回去。启动阶段同步写属性是安全的
  （core 空闲、无 DR 竞争，见"GUI 线程不得同步调 mpv API"的例外条款）。
- 枚举失败（非蓝光结构/损坏）→ 退回 `loadfile bd://`（mpv 默认播最长 title）。
- CLI 输入也改走 `viewModel.openFiles`（原先直接 `mpv.loadFile`），否则
  `vsr-player xxx.iso` 这条路径拿不到 ISO 特殊处理。

**实测（本机 `test.iso`，20.6 GB）**：卷标 `CW3DBD_04`（3D 蓝光，`BDMV/STREAM/SSIF/`
5 个 ssif），枚举 6 个 title（0:00 / 0:10 / 0:30 / 1:30 / 4:00 / **1:18:30**，
主片 mpls=00003、16 章），`aacs=0`；点击主片 → `bd://mpls/3`、
`h264 [High] 1920x1080`、`hwdec=nvdec`、`chapter-list/count=16`；重启后自动
恢复 6 条 + 设备。普通文件与目录播放未受影响。

**⚠ 该盘是双轨 MVC（蓝光 3D），只能播 base view（2D）**：SSIF 里两条视频 PID
（0x1011 base + 0x1012 dependent/MVC），依赖视图的 NAL 是 type 15（subset SPS）
与 type 20（slice extension）。FFmpeg 9.0.2 **不能解 MVC**（`sps_id 1 out of range`）。
开源方案存在但**未合并**：[FFmpeg PR #24303](https://patchwork.ffmpeg.org/project/ffmpeg/patch/20260828142448.29D46269EF@ffbox0-bg.ffmpeg.org/)
（Dom Cobley，2026-08，18 个补丁，`view_ids` 选视图），已知缺口正是我们要用的：
**mpegts 里两视图分属不同 PID、未做关联**（蓝光 .ssif/.MTS 仍只解被 map 的那路），
且 **hwaccel 下只解 base view**（必须软解）。真要支持 3D 需 backport 该系列 +
自己做 SSIF 双 PID 关联，见后续评估。

### 桌面集成（2026-10-17）

- `scripts/install.sh` 装应用菜单入口：入口内容 = **`data/vsr-player.desktop.in`**（单一事实源，中英 Name/Comment；`@EXEC@` 由脚本替换为安装后绝对路径，不依赖 PATH），图标 = `data/vsr-player.svg` + `.png` → hicolor `scalable`/`512x512`，之后 best-effort 刷新 `update-desktop-database`/`gtk-update-icon-cache`/`kbuildsycoca6`。打包脚本把 `data/` 放进 tarball 根（`build_release.sh` staging）。
- `main.cpp` 里 `QGuiApplication::setDesktopFileName("vsr-player")`：Wayland app_id / X11 WM_CLASS 钉成与 `.desktop` 同名，否则任务栏图标与窗口分组会失配。
- ⚠️ **图标是占位**（用户 2026-10-17："后续做个像样的"）：正式图标只需替换 `data/vsr-player.svg`（矢量单一事实源）并跑 `scripts/render-icon.sh` 重生成 PNG。
- `Exec=… %f`（单文件/目录）：2026-10-18 起 `Options::parse` 接受多个位置参数（= 播放列表条目，见上），所以 `%U` 已经可用；当前**没有**改 `data/vsr-player.desktop.in`（改它等于让"文件管理器多选"变成播放列表，需要时再定）。

### 构建

- **patch 方案**：`third_party/mpv`（纯净 0.41，只读基座）+ `src/mpv` 覆盖层（只含修改文件）。改 mpv 代码 = 改 `src/mpv/<同路径>` 后跑 `scripts/build_mpv.sh`（合并到 `build/mpv` 再 meson 构建）。
- **构建陷阱**：`build_mpv.sh` 输出被 grep 过滤，编译失败看不到——确认最后一行 "Done"；别 `cd build` 后还用相对路径；系统库升级（FFmpeg/Qt/TRT）后必须全量重建（soname 变更直接启动失败）。
- **standard / full 分层（2026-10-26 用户定，已实现）**：`build_release.sh` 两个变体
  按"谁依赖系统库"切分，**不是按体积随意砍**：
  - **standard（默认，6.3 MB tarball）**：只捆 `libmpv` + 光盘媒体栈（libcdio/libdvd/libbluray）
    + 我们自己的 wayland 模块；**FFmpeg ×7、CUDA runtime（libnvrtc/builtins/cudart）、
    Qt 一律用系统**。要求：系统 Qt ≥ 6.12、FFmpeg soname 与构建机一致（当前
    `libavcodec.so.63` 等 7 个）、CUDA runtime 可解析（本机 `/opt/cuda`，已在
    `/etc/ld.so.conf.d/cuda.conf` 里）。适合滚动发行版用户（系统本来就是新的）。
  - **full（~70 MB）**：在 standard 之上再捆 FFmpeg ×7 + CUDA runtime + 最小 Qt
    （库闭包 + 3 个平台插件 + wayland-* 插件 + `qml/QtQuick`(含 Controls/Dialogs/
    Layouts/Shapes)+`qml/QtQml`）；用户侧只需驱动 + Vulkan loader + 基础 C 运行库。
    面向 Qt < 6.12 / 老 FFmpeg / 无 CUDA toolkit 的发行版。**不对外分发**（按需构建）。
  - 体积参考（实测）：standard 6.3 MB；带 ffmpeg+CUDA 的 55.8 MB 版本里
    ffmpeg 占 12.6 MB、**libnvrtc 一个就占 36.9 MB（66%）**——真要瘦身先动 libnvrtc，
    不是 ffmpeg。
  - ⚠ 实现细节：① `4b` 的媒体栈发现必须**迭代到不动点**（`libmpv → libcdio/libdvdnav
    → libdvdread` 两层链；standard 去掉 libavformat 后只看 libmpv 的 NEEDED 会漏
    libdvdread，闭包自检直接失败）；② standard 的白名单要放行
    `libav*|libsw*|libnvrtc|libcudart`（与 `libQt6` 同位）。
  - **soname 不硬编码（2026-10-26）**：full 的 ffmpeg 收集列表改成从
    `libmpv.so.2` 的 NEEDED 推导（`grep '^lib(av|sw)'`），ffmpeg 大版本升级后重建
    自动跟随；standard 分支在构建日志里直接打印"要求系统提供: libavcodec.so.63 …"，
    清单永远与构建机一致，不会出现"脚本写死 63、实机是 64"的静默错配。
  - **装的时候就要能判断（install.sh [6/6] 预检）**：`ldd` 检查两个二进制，缺任何
    NEEDED → **明确报错并 exit 1**（附三条出路：装对应系统库 / 换 full / 源码构建），
    不再表现为"双击没反应"。另外强制校验 **libmpv 必须解析到本包的
    `lib/vsr-player/`**：系统若装了 mpv（`/usr/lib/libmpv.so.2`），捆绑库缺失时加载器
    会**静默回落**到它——程序能启动，但跑的是没有 `vf_vsr/vf_vfg/vf_rot` 和 libmpv vo
    互操作的原版 mpv，表现为"能开但超分/插帧全没了且无任何报错"（实测：挪走捆绑
    libmpv 后 `ldd` 解析到 `/usr/lib/libmpv.so.2`，并不报缺失）。
  - **依赖盘点（2026-10-26，逐类定论）**：`libmpv.so.2` / `mpv-vsr` 共 ~55 条
    `DT_NEEDED`，按"soname 会不会随上游跳"分四类：
    | 类别 | 例子 | 处理 |
    |---|---|---|
    | 每次大版本必跳 | ffmpeg ×7（`.so.63/.12/.61/.7/.10`） | standard 去版本化 + 安装时链接 |
    | 会跳但**不能**改 NEEDED | `libnvrtc.so.13`（符号版本命名空间就是 soname） | 保持带版本；靠系统，跳了由 compat 链接兜底 |
    | soname=API 版本、跨版本会**静默**串 | `libplacebo.so.360`（`pl_log_create` 的 api_ver 在库里是 "historical, ignored currently"，`/usr/include/libplacebo/log.h:69`） | **两变体都捆**（含 shaderc/glslang/SPIRV-Tools/dovi 闭包） |
    | 记成**绝对路径** | `libmujs.so`（无 SONAME → NEEDED 写成 `/usr/lib/libmujs.so`） | **两变体都捆** + `rewrite-needed.py` 改成 basename |
    | soname 长期稳定 | glibc / libX11 / libwayland / libcuda / libvulkan / `libQt6*.so.6` | 用系统（标准做法） |
    | soname 少见跳、无符号版本 | libass/libzimg/libluajit/librubberband/libuchardet/libsixel/libcaca | 用系统；真跳了 compat 链接兜底（它们无 VERNEED，改动安全） |
  - **为什么必须这么处理（引用）**：`DT_NEEDED` 记的是库的 **SONAME**
    （[Oracle: Recording a Shared Object Name](https://docs.oracle.com/cd/E19253-01/817-1984/6mhm7pl4u/index.html)、
    [Debian policy：库由 SONAME 标识](http://www.debian.org/doc/debian-policy/policy.pdf)），
    所以 `-lavcodec` / 无版本软链去链接**仍然**记 `.so.63`（实测 A/B）。真实世界的同类
    事故：[zen-browser #14889](https://github.com/zen-browser/desktop/issues/14889)
    （系统 ffmpeg 升 9.0 → `libavcodec.so.63` 不在白名单 → H.264 全挂）。
    生态里的两种正解：**重编译对齐目标系统**（发行版做法，
    [Debian #699648](https://alioth-lists.debian.net/pipermail/pkg-multimedia-maintainers/2013-February/031075.html)）
    或**打包时改写 NEEDED/整包捆绑**（[Nix auto-patchelf](https://git.qyliss.net/nixlib/blame/pkgs/build-support/setup-hooks/auto-patchelf.sh)、
    AppImage/manylinux 的 auditwheel）；发行版本身靠"soname 版本共存"（如 Ubuntu 的
    [libplacebo360](https://packages.ubuntu.com/search?lang=zh-tw&keywords=libplacebo360) 包）让不同代共享。
  - **发版规则与「组件版本表」（2026-10-26 用户定，已实现）**：
    **标准版不带任何第三方依赖**（只带我们自己的产物：`libmpv.so.2` + 2 个
    `libvsr-*.so`），**完整版（full）带全部依赖**（ffmpeg / CUDA / Qt / libplacebo
    及其 shader 栈 / libmujs / 光盘栈）。每次打包由 `scripts/gen-components.sh`
    从**产物本身**推导出 `COMPONENTS.md`（readelf NEEDED + 构建机解析结果，
    不是手写清单，所以不会过期）：
    - standard 那张表 = "**要求系统提供**"（61 条 NEEDED，逐条给 SONAME + 构建机
      实际版本，如 `libavcodec.so` ← `libavcodec.so.63.1.102`）；
    - full 那张表 = "**本包自带**"（27 个库逐条列版本/大小）+ 目标机额外要求；
    - 两张表都带外部组件（驱动 / VFX SDK 版本 / Vulkan / Qt 要求）与排障命令。
    `install.sh` 同时把 `COMPONENTS.md` 装到 `~/.local/share/vsr-player/`——
    排障时让用户直接发这个文件即可。
    ⚠ 连带改动：闭包自检对 standard 放宽成"构建机能解析即算系统提供"（否则按新
    规则每个第三方库都会报未捆绑）；`soname-compat.sh` 加黑名单——**libplacebo /
    libnvrtc / libcudart 禁止自动兼容链接**（跨 major 是静默不兼容，`ldd -r` 也
    查不出来），缺了就明确报错并建议用 full。
  - **跨 major 的可见性（2026-10-26）**：去版本化后 NEEDED 里没有 major 了，而
    跨 ffmpeg major **没有 ABI 承诺**（符号齐全 ≠ 结构体/语义一致，`ldd -r` 查不出来）。
    所以 `build_release.sh` 在 staging 里写 `lib/vsr-player/VSR-FFMPEG-MAJOR`
    （从 `$MPV_DIST/_build/libmpv.so.2` 的 NEEDED 读，注意 staging 早于 `needed_of()`
    定义 → 用 readelf 直读），`install.sh [6/6]` 拿它对照目标机实际
    `libavcodec.so.<N>` 的 SONAME：一致打 `✅ FFmpeg 主版本匹配`，不一致打 ⚠ 并提示
    "符号能对上不代表行为对，要确定性用 full 变体"（实测两支都验过）。
  - **FFmpeg NEEDED 去版本化（2026-10-26，用户要求；standard 变体）**：
    `scripts/rewrite-needed.py` 在打包时把 `lib/vsr-player/libmpv.so.2` 和 `mpv-vsr`
    的 `DT_NEEDED` 从 `libavcodec.so.63` 改成 `libavcodec.so`（原地截短 .dynstr 里
    那个字符串），安装时由 `soname-compat.sh` 按**目标机实际版本**建
    `$LIB_DIR/libavcodec.so -> /usr/lib/libavcodec.so.<N>`。这样包本身不再钉任何
    FFmpeg 版本。⚠ 三条实测结论：① 链接器无法绕过——`DT_NEEDED` 取库的 SONAME，
    用 `-lavcodec`/无版本符号链接去链接仍记 `.so.63`（Oracle "Recording a Shared
    Object Name" / Debian policy 同款规则）；② **`libnvrtc` 绝对不能改**：NVIDIA 的
    符号版本命名空间就是 soname 本身，截短会连带改掉 version tag → 加载器报
    `version 'libnvrtc.so' not found` + 所有 `nvrtc*` 符号 undefined（实测）；
    FFmpeg 的 version tag 是 `LIBAVUTIL_61` 这类，与 soname 无关，且 NEEDED 与
    VERNEED 的 `vna_file` 共用同一偏移、同步截短后仍一致 → 安全（`readelf -V` 可验）。
    ③ 脚本内置 VERNEED 守卫：任何同时是 `vna_name` 的字符串一律跳过。
  - **soname 兼容软链（2026-10-26，用户提的"软链就行"是对的）**：
    `scripts/soname-compat.sh`（随 tarball 发布，install.sh [6/6] 在严格匹配失败后调用）：
    缺 `libX.so.N` 时在 `$LIB_DIR`（RPATH 首位）建 `libX.so.N → 系统里同库的其它 major`
    （候选来源 `ldconfig -p` + `LD_LIBRARY_PATH` 目录，取 major 最大者），然后
    **`ldd -r` 验证没有 `undefined symbol`** 才保留；有缺符号就**回滚软链并 exit 1**。
    要点：跨 major 是 ABI 赌注，但失败面是"加载器报 undefined symbol"（可检测），
    而不是"启动成功、功能悄悄坏掉"。单测（合成 libfoo.so.1 → 只剩 .so.2）：
    ABI 兼容时自动建链 + 程序正常；把新库的符号删掉后能抓到 `undefined symbol: foo`
    并回滚。**这条让 standard 在系统 ffmpeg 大版本升级后不会直接死**——能兼容就自动补。
  - **soname 绑定的本质**：ELF 的 NEEDED 记的是 so 名字，ffmpeg 跨大版本没有 ABI
    兼容承诺 → **不捆就必然绑定某个 soname**。根治只有两条路：捆库（= full，
    ffmpeg 12.6 MB 压缩）或静态链接 ffmpeg（Arch 不提供 .a，要自建 ffmpeg，
    体积更大且每次 ffmpeg 安全更新都要重编）。因此**对外分发用 full**；
    standard 的定位是"与构建机同源"的薄包。
  - ⚠ dev 模式 `install.sh` 与 release 包**语义不同**：dev 把构建树产物装进 `~/.local`
    （RUNPATH 指向 `build/mpv/_build`，依赖构建树）；release 包是
    `$ORIGIN/../lib/vsr-player` 相对 RPATH，自洽。用户要"正式安装"就走
    `build_release.sh` + 包内 `install.sh`。
- 分发（2026-09 重构）：**源码构建为推荐路径**——`scripts/build-from-source.sh`（依赖自检→构建→安装→记录快照）；系统库升级后重跑同一条命令即可恢复。预编译 tarball 两个变体：`build_release.sh`（standard，需系统 Qt ≥6.12——Qt 二进制兼容是单向的，构建机 Qt 6.12.0 → 真实下限 6.12）与 `--variant=full`（捆绑最小 Qt：镜像发行版布局 `<prefix>/lib/qt6/{plugins,qml}` + Qt 库在 `<prefix>/lib/`，靠 Qt 自带相对 RUNPATH 解析，**免 patchelf**；`main.cpp` 在 QGuiApplication 前探测并设 `QT_PLUGIN_PATH`/`QML2_IMPORT_PATH`）。依赖收集 = 显式核心集 + 媒体栈动态发现（libcdio/libdvd/libbluray）+ **打包时闭包自检**（未捆绑且非白名单即失败）；`--engine=<已构建引擎>` 跳过 trtexec。
- **依赖漂移检测**：`scripts/check-deps.sh [--record]` 记录构建时**系统库**快照（路径+mtime+size，排除仓库内自建产物——否则每次重建都误报），系统升级替换后提示重建；staging 目标用 `LD_LIBRARY_PATH` 模拟安装态解析。**soname 变更导致启动失败 → 跑 `build-from-source.sh` 重建**；用户侧排查见 README「故障排查」。
- `scripts/install.sh`（用户端）：~/.local；VFX 从 **NVIDIA 官方索引 `pypi.nvidia.com`** 取 nvidia-vfx wheel（pypi.org 仅占位 sdist，2026-09 实测），curl 提取不调 pip 不改系统；VFX ~0.6GB 不随 tarball 分发（SLA）。⚠️ `set -euo pipefail` 下所有探测命令（nvidia-smi / pkg-config / curl 管道）必须 `|| true` 兜底，否则"未检测到"会变成"安装静默中止"（本项目实测踩过两次）。

## 环境速查

- GPU：RTX 5060 Ti（Blackwell sm_120）；驱动 615.78.08（2026-10-17 实测）；KWin Wayland 6.8.0（纯 Wayland，无 X11）。显示器 DP-3 = LG ULTRAFINE 27" 4K：EDID 含 ST2084、峰值 400 nit、基色≈DCI-P3（HDR 可在 KWin 里开）
- Qt 6.12.0 / C++20 (GCC 13+) / Meson / FFmpeg 9.0 / TRT 11.2.1.2（pacman）
- 测试文件：`input/`（catlove_720p.webm、Jellyfish 1080p h264/h265、goose_480p、heya 等）
- **软件解码能力（2026-10-26 实测，i5-14600KF）**：`input/Fallout … 2160p … H265`（3840×1600
  HEVC Main10 HDR，24fps）用 `--benchmark --scale off -- --hwdec=no` 跑出 **176 fps**
  （约 7.3× 实时，CPU 约 5.6 核）；`ffmpeg -benchmark` 独立对照 **208 fps / 8.66×（≈7.4 核）**。
  同文件 `--scale 2`（4K→7680×3200）46.8 fps、`--fruc 60` 168 fps、`--scale auto` 176.9 fps
  （benchmark 无窗口 → auto 判定 1× 直通）、`--fruc 120`（4K→120fps）**168.3 fps**。
  结论：**4K 软解在本机有 7–8× 余量 → 4K60（2.9×）甚至 4K120（1.4×）都不缺解码能力**，
  4K 内容播放时 `scale=auto` 天然是 **1× 直通**（VSR 不参与，无额外成本），所以
  "4K 播放"在本机**没有瓶颈**。（`--scale 2` 的 46.8 fps 只是人为构造的 4K→8K 压力测试，
  真实场景不会出现——4K 已经到头了，没有上采样需求。）
  所以 1080p×2 的 MVC（≈2×1080p H.264 ≈ 4.1 Mpx/帧）更是远在余量内，连假设的 4K MVC
  （2×4K 解码 ≈ 88 fps 等效）在 CPU 侧都够 24fps。
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

## 待实现需求（用户已提出，**暂缓、不要动手**）

### 单眼 2D 输出（vf_eye，**已实现 2026-10-27**）

**需求**：不看 3D 时，主窗口按**单眼正常画幅**满屏播放，且**超分按单眼算**
（VSR 输入 1920×1080，不是整帧 SBS）。

#### 实现

- **新滤镜 `vf_eye`**（`src/mpv/video/filter/vf_eye.c`，CUDA 零拷贝，仿 `vf_rot`）：
  - 选项 `mode=off|left|right`、`layout=sbs|tab`、`half=0|1`；
  - **全幅 = 1:1 拷贝裁半幅**；**半幅 = 裁半幅 + 沿 split 轴重采样回自然画幅**
    （SBS 半 960×1080 → 1920×1080；TAB 半 1920×540 → 1920×1080）。
    Y 用双线性（scale=1 时退化为精确拷贝），UV 用最近邻（NV12/P010 的 U/V 是
    pair，不能把 pair 当整数插值——进位会串）；
  - 输出帧尺寸 = **单眼画面**：`sbs 全 →(W/2,H)`、`sbs 半 →(W,H)`、
    `tab 全 →(W,H/2)`、`tab 半 →(W,H)`；hwctx 按输出尺寸自建
    （借用源尺寸会错乱/崩溃——`vf_rot` 的旧坑）；
  - 运行期命令：`vf-command eye <off|left|right>` / `vf-command eye stereo <sbs|tab> <full|half>`。
- **链上位置**：`hwup → eye → rot → vsr → truehdr → vfg`（`vfOption()` 的**三个 return
  分支**都插入了——VFG 主链、`--fruc-order pre` 链、RIFE 链；漏一个就会出现
  "eye 滤镜根本不在链上"而 vf-command 静默失败）。
- **⚠ 半幅必须真拉伸，不能只设像素宽高比**：底栏「画面比例」用 mpv
  `video-aspect-override`，一旦用户设了固定比例就会覆盖 p_w/p_h → 半幅被压扁；
  而且 `vf_vsr` 的 `scale=auto` 按帧 w/h 算倍率（960×1080 会算偏大）。
  烘焙成标准单眼帧后，比例/旋转/VSR auto/VFG 全按普通 2D 工作，客户端零特例。
- **客户端**：`PlayerViewModel::setEyeState(mode, layout, half)`（layout/half < 0 =
  保持当前值）；链串带 `@eye:eye:mode=…:layout=…:half=…` 初值；
  `syncVfOptions()` 与 `replayStereoMap()` 都会重放（**链重建抹运行期 vf-command**）。
- **UI**：右键菜单一级的「3D」小块 = 三条入口
  **输出方式**（关闭 / 眼镜 3D / 单眼 2D）、**片源格式**（左右·全/半、上下·全/半）、
  **输出选项**（左眼 / 右眼）；输出方式里第三项文案 = **2D 输出**（不是"单眼 2D"）。
  ⚠ **三条目彼此独立，不做联动**（2026-10-27 用户明确要求）：
  - 不加 `visibleWhen` 隐藏、不按模式切标签、不因某个选项自动改另一个选项；
  - 输出选项只写"单眼输出用哪只眼"这一个独立状态 `eyePref`（0=左 1=右），
    即使输出方式是"关闭"也能选（下次进单眼模式生效）；
  - 唯一不可避免的耦合是"输出方式"本身是个三态开关：选眼镜 3D 会 `stereo.start`
    （并 `stereo.stop()` 掉单眼）、选单眼 2D 会 `stereo.stop()` 并开单眼——
    这是模式切换的语义，不是跨选项联动。
  - 眼镜模式下的"交换眼序"只由 `K` 快捷键控制，不放进菜单。
- **快捷键**：`J`/`L`（布局、全半）、`K`（眼镜=交换眼序 / 单眼=换眼）在**两种模式下都生效**；
  `A`/`Shift+A` = 画面比例循环（列表照抄 mpv，见下）——3D 下也能用（戴眼镜直接调）。
- **片源格式自动判定**：按**原生像素尺寸**（`video-params/w,h`，不能用 aspect——会被
  override 污染）：宽/高 ≥ 2.6 → 左右·全；**≤ 1.25 → 上下·全**（1920×2160 = 0.889；
  4:3 = 1.333 不会误判）；其余不猜（半幅无法从尺寸区分）。结果经**粘性属性**
  `sourceLayoutHint` 给 QML（判定可能发生在 QML 就绪前，`Component.onCompleted` 补应用）。
- **布局/全半是片源属性，但"不跨会话保留"**（用户 2026-10-27 定；要做按文件保存才会持久化）：
  `StereoController` 构造时一律 `layout_=0 / half_=false`，**不读也不写** `stereoLayout`/
  `stereoHalf`；`main.cpp` 启动时用独立 QSettings 一次性清掉旧版本遗留的这两个键
  （放在最前面 + `sync()`：两个控制器各有 QSettings 缓存，从别处 remove 会被它们
  随后的 sync 写回去）。打开文件时的自动判定仍然生效（只在内存里改）。
  `start()` 也不重置（进 3D 不该清掉当前片源格式）。
- **RPC**：`set-eye off|left|right [sbs|tab] [full|half]`、`get-eye`、
  `get-video-size`（滤镜链输出帧尺寸，验证裁切/倍率不用截图）、
  `get-source-format`（layout/half/eye/hint 状态）。

#### 验证（headless，`--benchmark` + RPC，不需要上屏）

| 操作 | `get-video-size` |
|---|---|
| SBS 3840×1080 整帧 | 3840×1080 |
| `set-eye left sbs full` | **1920×1080** |
| `set-eye right sbs full` | **1920×1080** |
| `set-eye left tab full`（对 3840×1080） | **3840×540** |
| `set-eye left sbs half` | **3840×1080**（拉回帧尺寸） |
| TAB 1920×2160 + `set-eye left`（自动判定 layout=tab） | **1920×1080** |

#### ⚠ 构建陷阱（踩过）

`ninja -C build` **不会**重编 `src/mpv/` 的 overlay：libmpv 由 `scripts/build_mpv.sh`
单独构建（输出 `build/mpv/_build/libmpv.so`，370 个目标）。改了滤镜必须：
`./scripts/build_mpv.sh && ninja -C build`。否则表现为"代码明明改了、链里没这个滤镜"。
另：新增 QML 文件必须登记进 `src/client/resources.qrc`，否则启动即失败（QML 类型不存在）。

## 相关文档

- `README.md`/`README_zh.md`（功能/构建/分发/CLI）、`docs/`（设计记录与参考索引）。调试守则/方法论在 `~/.dsh/AGENTS.md`（全局）
- **`docs/memory/`** — 历史调研/调试记录（源自 Claude Code 会话记忆，筛选保留 6 个知识类主题 + `README.md` 索引；过程性/过期记录已移除）。⚠️ 部分内容已过时（如 RIFE 4.25、闪回未解决），引用前对照代码，以本文件为准
