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
   `/usr/lib/qt6/bin/lupdate <main.qml ui/*.qml ui/components/*.qml PlayerViewModel.cpp> -ts
   src/client/translations/vsr-player_{en,zh_CN}.ts`，再补两份译文并 `ninja -C build`
   （meson 的 custom_target 跑 `lrelease` → `<build>/src/client/*.qm`，`main.cpp` 按 locale
   经 `VSR_TRANSLATIONS_DIR` 载入；日志会打 `translation: vsr-player_zh_CN`）。
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

### RIFE 插帧（重点，状态最新）

- **引擎 = RIFE full FP16 唯一引擎**（`rife_full_fp16.engine`，动态 shape 7ch）——lite 已彻底拆除（FP16 连续推理 ~6 次后 NaN 退化，实证与 TRT 版本无关）；`--rife-model` 参数已删；`vf_rife.c` 无 variant/lite 残留。
- **模型 = RIFE 4.26 v2**（7ch `rife_v2` 变体，`third_party/rife/rife_v4.26_v2_fp16_all.onnx`）——**不是 4.25**（旧记忆仍写 4.25，过时）。切换理由：质量全档胜出（合成 GT PSNR +1~4.6dB，中位移优势最大）。构建：`bash tests/fruc/build_rife_full_engine.sh`（系统 trtexec，动态 profile FP16，`--hardware-compat on` 跨架构 ampere+）。
- **TRT 版本绑定**：引擎内嵌构建时 TRT 精确版本，升级 TRT 后必须重建引擎。当前系统 = **TRT 11.2.1.2**。
- **闪回问题已解决**（`vf_rife.c:622-694`）：整数倍率 ≥2 用 vs-mlrt `Interleave([src, interp])` 语义——源帧总保留、插值帧在相对时间点 t=i/k（tval 恒 i/k，无极端 tval）；**PTS 打在帧的内容时间**（mid 在 `t_prev + tval·(t_cur−t_prev)`、cur 副本在 `t_cur`）——严格单调、无重复无空洞（音频同步下视频间隔=源间隔/k，与音频时钟零漂移）。⚠️ 旧方案（绝对网格 `t0+n/out_fps` + llround 锚定，每对固定 k 帧）在源帧率不是网格整数倍时（如 29.97→60 比例 2.002）会周期性重复/空洞网格点 → mpv `Invalid video timestamp` + 16s 级 desync + 持续丢帧（2026-08-18 test.wmv 播放速度异常，已修）。非整数倍率（<2× 或 24→60=2.5）保留绝对网格（floor 半开区间，无重复但 VFR 源有极端 tval 风险，可接受）。根因背景：绝对网格在 VFR 源（goose 33.0/34.0ms 交替 PTS）上使 tval 摆到 ~0.01–0.99 → 极端 tval 插值 + 源帧被吞 → 内容抖动（此问题由固定 tval 内容解决，与 PTS 方案解耦）。极端 tval（<0.05/>0.95）复制近端点。
- **UI 档位 = [Off, 40, 48, 60]**（`QualityPopup.qml`）——30 已移除、48 已添加；CLI `--fruc` 接受 `off/40/48/60/2/3/4`（2/3/4 为 benchmark 倍率）。**`--fruc 30` 已不接受**。
- **VFG 输出帧率上限 `max-fps`（2026-10-20，仅 VFG）**：用户显式上限而非性能自动降级。
  `QualityPopup` 的 "Max interpolation frame rate" 排（48/60/120 fps，默认 **60**；`--fruc-max-fps` /
  RPC `set-fruc-max-fps` / 持久化键 `frucMaxFps`；RIFE 分支该排隐藏、参数忽略）。
  filter 判据（`vf_vfg.c` 直通判定）：`k × src_fps > max_fps` → `vfg-status: mode=
  passthrough reason=max-fps`；**恰好等于上限允许插帧**（24×2=48 @cap48、30×2=60 @cap60）；
  `src_fps<=0`（直播流）不判定；上限在引擎初始化前判 → 被限制时不建引擎/不耗 CUDA；
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
- **验证证据**：合成 BT.2020/PQ 三色条（420）ours vs mpv 原生路径逐通道吻合（148.0/0/40.8 vs 145/0/39.7 等）；SDR 8-bit 仍是 `cuda[rgba]`、HDR/10-bit SDR 是 `cuda[x2bgr10]`。脚本：`build/scratch/{vsr10_probe.c,hwf_test.c,pixdesc.c}` + `hdr_bars420.mp4`/`hdr1080p_test.mp4`（均为 gitignore 的临时产物）。⚠ 上述"逐通道吻合"是**修 limited-range 之前**的截图口径（8-bit 屏幕截图会把码域差异压缩）；修正后对**参考值**重新对账见 §19（旧内核 vs ffmpeg 参考 mean|Δ|=23.8/max=79 10-bit LSB，新内核 0.98/3）。
- **已知遗留（未修，均有实测证据）**：① ~~`yuv_to_rgba` 的色度按亮度 span 归一化~~ **limited-range 归一化已修（2026-10-17，见 `docs/hdr-pipeline-investigation.md` §19）**——此前 luma **与**色度都用满幅跨度（8-bit 239、P010 61439）而非 limited 跨度（219/224、56064/57344），白位只送到满幅的 91.6%（8-bit 白 234/255、P010 白 934/1023、HDR 条红 941 vs 参考 1010）。⚠ 该修正**同样改变 SDR limited 片源的观感**（白位 234→255），属预期（对齐 mpv/ffmpeg，非回归）；② 4:4:4/4:2:2 hw 帧被 kernel 当 4:2:0 半平面读（合成测试暴露，8-bit 同样存在）；③ `-Dfruc_backend=rife` 分支的 RIFE 冻结在 8-bit（`yuv_to_rgba_init(..., VFX_BITS_8)`），RIFE 链（rife→vsr）因此恒 8-bit——VFG 链才走 10-bit。
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
- **探测链路已落地**（2026-10-17；数据见 `docs/hdr-pipeline-investigation.md` §12/§15）：
  ① `VulkanContext` instance 加 `VK_EXT_swapchain_colorspace`（先枚举可用性，缺了只记
  日志、不让 vkCreateInstance 失败）；② `src/client/HdrOutput.{h,cpp}` 首帧
  （`afterRendering`）探测 `QRhiSwapChain`（`isFormatSupported/format/hdrInfo`）
  + `screenChanged` 重探；③ `src/client/WaylandColorFeedback.{h,cpp}` →
  `libvsr-color-feedback.so`：经 Qt 的 wayland 连接读 **surface preferred image
  description**（`wp_color_manager_v1.get_surface_feedback`）→ 显示能力 + 异步变化
  回调（转投 GUI 线程）。**旧的 output-description 模块
  `WaylandColorManagement`/`libvsr-color-wayland.so` 已删除**。
- ⚠ **两条被实测推翻的判据**（别再走回头路）：
  1. `isFormatSupported()` 在 Wayland 上是"合成器整体支持"——SDR 屏（DP-2）也返回
     HDR10=1，**不能**判断"这块屏是不是 HDR"。
  2. **output image description 也分不出来**：同一块 LG 在 HDR 开与 WCG-only（HDR 关、
     WCG 开）下，output description **逐字段相同**（bt2020/gamma2.2/0–400/ref400）。
     能分辨的是 **surface preferred**：HDR 开 → `bt2020`（gamut_area 0.2119）；WCG-only
     → `bt709`（0.1120）；SDR 屏 → `bt709`（0.1120）。`reference_lum` 还会在
     `max/π` 与 `sdrBrightness` 之间漂（63/124 vs 200/400）→ mpv 的
     `max_luma > ref_luma` 判据在本机不成立。
  → 判据取 **"transfer 是 PQ/HLG 或 基色三角面积 > 1.15×sRGB(0.112)"**；读不到一律退 SDR。
- ⚠ **Qt 的便利类没导出**：`QtWaylandClient::ColorManagementFeedback` /
  `ImageDescriptionInfo` / `ColorManager` 无 export 宏（连 `staticMetaObject` 都拿不到）
  → dlopen 会 `undefined symbol`。只能用**导出的协议封装**
  (`QtWayland::wp_color_management_*`) 自己写 listener/解析。
  ⚠ 初始 preferred **必须主动 `get_preferred()`**（协议只在变化时发事件）。
  KWin 支持 PQ（`wp_color_management_v1 v2`，无 icc；`kscreen-doctor -o` 看真实状态）。
- **运行期重新声明色彩空间已验证**：`QWindow::setFormat(colorSpace=BT2020/ST2084)` +
  调 `QtWaylandClient::QWaylandWindow::initializeColorSpace()`（导出符号，模块里可调）
  → `WAYLAND_DEBUG` 实测发出 `create_parametric_creator` + `set_primaries_named(6)` +
  `set_tf_named(11)` + `ready` + `set_image_description`。swapchain 侧的运行期切换
  （`sc->setFormat`+`newCompatibleRenderPassDescriptor`+`createOrResize`，Qt 的
  per-frame RT 会在 createOrResize 里刷新）与自动决策**现已实现**（见下面"HDR10 上屏
  已落地"与 §16）。
- Qt `hdrInfo()` 在 Linux 恒为硬编码默认值（min 0 / max 1000 / sdrWhite 200），已实测
  两屏一致 → 只能用于"证明它不可用"。
- **HDR10 上屏已落地（含自动决策与运行期切换）**（2026-10-17；env
  `VSR_HDR=off|auto|hdr10`，**默认 auto**；细节/证据见 `docs/hdr-pipeline-investigation.md`
  §14/§16）：
  - 渲染链路：rtImage/composite attachment/mpv wrap 全走
    `A2B10G10R10_UNORM_PACK32`（`Video::setRenderFormat` +
    `CompositePipeline::create(..., VkFormat)`，可重入）；mpv overlay 的
    `mpv_vulkan_image` 加 `hdr/min_luma/max_luma` → 填 `ra_fbo.color_space`
    （PQ/BT.2020 + 合成器亮度）；`vk_color_depth()` 按格式 10/16/8。
  - **auto 判据**：内容 HDR（mpv `video-params/gamma` ∈ pq/hlg，`PlayerViewModel::videoHdr`）
    × 显示能力 HDR（surface preferred 的 `gamut_area>1.15×sRGB` 或 PQ/HLG）→ 才切
    HDR10；读不到 / SDR 屏 / WCG-only / SDR 内容 → 保持 SDR（**off 策略完全不碰
    色彩空间声明，逐字节现状**）。
  - **运行期切换 `hdr::applyMode`**：属性 + `QSurfaceFormat::colorSpace` +
    `redeclareWindowColorSpace()`（重下发 wp_color_management）+ `QRhiSwapChain::
    setFormat/newCompatibleRenderPassDescriptor/createOrResize()` → 再走
    `rebuildRenderPath`（重建 composite pipeline + rtImage 格式）与 `updateMpvTarget`。
    实测双向：SDR→HDR10 与 HDR10→SDR（RPC loadfile 切片源）均成功，
    `libmpv_vk`/rtImage/mpv target 全部跟随，帧继续（25fps），校验层 VUID 集合
    与纯 SDR 运行**完全一致**。
  - ⚠ **踩过的真 bug**：`CompositeRenderNode` 曾经只在创建时缓存 pipeline 句柄 →
    运行期重建后，在没有 `updatePaintNode` 的帧里绑定了**已销毁的 pipeline**
    （`VUID-vkCmdBindPipeline-pipeline-parameter`）。修法：节点持 `Video*`，
    **渲染阶段**从 owner 读 `compositePipeline()/compositePipelineLayout()`。
  - **目标色彩空间必须由宿主用 mpv 选项声明**（`target-trc=pq`/`target-prim=bt.2020`）：
    generic libmpv 路径（`video/out/gpu/libmpv_gpu.c` 的 `render()`）自己拼 `ra_fbo`
    **丢掉 backend 填的 `ra_fbo.color_space`** → gl_video 把 PQ 内容烘成 gamma2.2 写进
    PQ swapchain（暗 + 高光过饱和 = 用户报告的"偏红"）。**不要**把合成器亮度缩放后
    填 `ra_fbo.color_space.hdr.max_luma`（会预 tone map 到 203nit，暗 2.4 倍）。
  - **是否自己下发 `wp_color_management` 描述 = 一个真实观感变量**（`VSR_HDR_DECLARE`）：
    **默认 1 = 自己声明**（bt2020 + st2084_pq + luminances 0/10000/203，mpv PQ 分支语义）
    —— 用户目检"画面正常"；`VSR_HDR_DECLARE=0`（= mpv `--target-colorspace-hint` 的
    **未声明**路径）在本机**画面发灰**（PQ 缓冲被按 gamma2.2 解释）。
    ⚠ 我一度按"同尺寸 meanLum（声明 13.1 / 不声明 19.6 / mpv 20.7）"选了"不声明"，
    结果发灰 —— **平均亮度不是正确性判据**（发灰恰恰均值更高）；必须目检，
    或看饱和度/对比度。测量口径要求（同窗口尺寸、播放中截图、挖掉中心区，
    避免暂停时的播放按钮 overlay 污染）仍然成立。
  - **HDR10 静态元数据已下发（2026-10-17，见 `docs/hdr-pipeline-investigation.md` §20）**：
    声明里补 `set_mastering_luminance` + `set_max_cll`/`set_max_fall`（来源 mpv
    `video-params/{min-luma,max-luma,max-cll,max-fall}`；合法性照 mpv
    `cm_metadata_sanitize()`；元数据变化时重下发）。**为什么必须有**：协议规定 PQ 下
    `set_luminances` 的 max_lum **被忽略**（恒 min+10000），内容峰值只能靠这三个字段；
    KWin `src/core/colorpipeline.cpp` 的 tone mapping 输入峰值 = `max_cll` → mastering
    max → 主色体积上限（=10000）。缺它 → 真实片源（如 mastering 1000nit/MaxCLL 758nit）
    被按 10000nit 压 → 对比度变平（用户原话"mpv 对比度更好"的根因）。
  - ⚠ **两个强制模式（`VSR_HDR=hdr10`）踩过的坑（2026-10-17 修，见 §21）**：
    ① `reevaluate()` 的"模式没变就早退"会漏掉 `updateMpvTarget` 钩子 → mpv 没设
    `target-trc=pq` → gl_video 不做 SDR→PQ 转换 → SDR 码值写进 PQ 面 = 颜色错。
    修法：模式未变也要 `syncTargets()`（幂等）+ 首帧补一次 `reevaluate`。
    ② **一个 wl_surface 只能有一个 color management surface**（协议 surface_exists）：
    show 前设 `QSurfaceFormat::colorSpace` 会让 Qt 先建一个，我们模块再
    `get_surface()` = 协议错误、**连接断开 app 退出**。修法：强制模式只设
    `_qt_sg_hdr_format`，colorSpace 与描述一律在首帧后由 `syncTargets()` 下发
    （自动切换路径本来就是这么做的）。
  - **OSD 有 `HDR` 状态行（2026-10-17，见 §22）**：`hdr::statusText()` →
    `PlayerViewModel::setHdrStatus()` → `osdTextString()` 的 `HDR` 行。
    形态：`SDR · content SDR` / `HDR10 · BT.2020/PQ · display 400nit · content 758nit` /
    `SDR · content HDR not displayed (display <原因>)` / 强制时带 `(forced)`。
    `content Xnit` 取 MaxCLL（退回 mastering max），`display Xnit` 取合成器 preferred 的
    max_luma（物理峰值）。正常播放 **Tab** 显示 OSD（`osdPoll` 仅在可见时推送）。
    ⚠ 切 HDR10 与静态元数据到达可能同瞬间 → **`setContentMeta()` 里必须刷新状态文本**
    （否则 OSD 停在 `content ?`）；内容 HDR 但元数据未到时显示 `content HDR`。
    无参数启动→恢复播放列表→点播放（上次记录续播）路径已实测同样正确。
    ⚠ **`Output` 行带实际输出模式**（2026-10-18 用户："OSD 缺少实际输出的信息，HDR10/SDR"
    → 追问后定为"实际输出合并进输出行，HDR 行信息完备"）：`输出  2560×1440 148.1fps · HDR10`
    / `· SDR`。数据源 `hdr::outputText()`，取值 = **真实 swapchain 格式**（首帧
    `installSwapchainVerification` 的 `probeSwapchain().current` + 换屏重探 + 每次
    `applyMode` 成功后），**不是策略意图**——Qt 静默回退时以探测值为准（回退本身由
    `installSwapchainVerification` 报 ERR）。分工：**HDR 行 = 内容侧**（内容 HDR？上变换？），
    **Output 行 = 显示输出侧**。推送同 `hdrStatus_`（GUI 线程写/事件线程读 + 锁）。
  - **`VSR_HDR=hdr10` 的定位 = 诊断/覆盖，非观看模式**（§22.1）：只用三类场景
    （机制验收/回归、auto 判据失灵时的逃生舱、分离"谁做 tone mapping"）；
    播 SDR 内容会更暗（SDR 白 203nit vs 面板 400nit，且无反相 tone mapping）；
    **不暴露到 UI**。
  - 仍未做：HDR10 下 Qt 自己的 UI 观感需目检
    （Qt Linux 用硬编码 1000/200 nit 假设）；`vk_color_depth()` 的 dither 深度在
    "只切换不 resize"时可能滞后（仅 dither）。
- ⚠ **`peakNits` 是参考白口径**（mpv/libplacebo `PL_COLOR_SDR_WHITE=203` 缩放后的
  `pl_color_space.hdr.max_luma`，DP-3 的 400nit 屏 → 203.0），不是屏的物理峰值——填给
  libplacebo/mpv 是对的，别把它当 nit 显示值。
- **已定**（本轮实验 + 实现，§15/§16）：WCG-only（HDR 关、WCG 开）的 surface preferred
  是 **bt709**、与真 HDR 的 bt2020 可分 → 判据不误判；**内容驱动**（只有 HDR 片源用
  HDR10，SDR 片源保持 8-bit 现状）+ 运行期切换（切换代价已实测：一次 swapchain 重建，
  无新校验错误、帧继续）。

### SDR→HDR10 上变换（VFX TrueHDR，2026-10-18 第一轮，**默认关**）

调研/方案/验收全文见 `docs/vfx-truehdr-investigation.md` §8；要点：

- **位置**：链上**恒在 `vf_vsr` 之后**（post 序 `hwup → vsr → truehdr → vfg`；RIFE 链尾）。
  `--fruc-order pre` **不插**（那等于放 VSR 之前，按输出帧计费 = 最贵方案）→ 记一行日志。
- **开关**：链串里恒 `@truehdr:truehdr:enable=no`（占位，直通成本≈0）；实际状态由 HDR 策略经
  `vf-command truehdr enable|luminance` 下发。**opt-in 环境变量 `VSR_UPHDR=on|1|true`**
  （默认关：收益未证实 + 成本吃紧）。⚠ 链串里 **`enable=no` 不能写 `enable=0`**——
  `OPT_BOOL` 只认 yes/no，写错会让**整条 `vf` 选项解析失败**（所有滤镜失效，现象是 VO 直通
  + A/V 不同步，mpv 只在 stderr 打一行）。
- **判定（真值表，单一事实源在 `HdrOutput`）**：
  `Enable = 内容 SDR ∧ opt-in ∧ 策略≠off ∧ 链上有 truehdr 滤镜`。
  ⚠ **2026-10-18 改（用户："面板非 hdr 也允许开启 sdr -> hdr10"）**：判据里**去掉**了
  "目标模式 = Hdr10"——上变换**与显示能力解耦**。面板非 HDR 时输出面仍是 SDR
  （不动 swapchain、不下发色彩描述），TrueHDR 产出的 HDR10 帧由 mpv 按其 target
  （`target-trc=auto` = SDR）tone map 回 SDR 写进现有 8-bit 面 = 改动前所有 HDR 片源
  走的那条通路。用户在两方案里选了 A（**不**放宽 swapchain/声明；B = 非 HDR 面板也
  强制 HDR10 上屏，未采纳）。auto 策略因此两格都通：**SDR 内容 × HDR 屏 × opt-in → Hdr10**
  （顺带切 HDR10 上屏）与 **SDR 内容 × SDR 屏 × opt-in → Sdr**（只跑上变换）。
  "链上有滤镜"一项由 `hdr::setUpconvertInChain()` 承担（`initTrueHdr` 按 `frucOrderPre()`
  设，**须在 installPolicy 前**）——`--fruc-order pre` 不插滤镜，不停这一步 OSD 会声称
  `SDR → HDR10` 而帧根本没被处理。
  过滤器里还有**第二层按帧 transfer 的自守卫**（`pl_color_space_is_hdr()` → 直通），
  所以内容 HDR 时即使客户端误开也不会二次转换。
- **OSD 的内容标记**（`HdrOutput::statusText()`，英文不翻译）：
  `HDR10 · BT.2020/PQ · display 400nit · content SDR → HDR10(400nit[, manual])`（HDR 输出 + 上变换）
  / `SDR · content SDR → HDR10(400nit[, manual])`（**面板非 HDR**：上变换在跑、输出仍 SDR）
  / `content 758nit`（真 HDR 片源，形态不变）/ `SDR · content HDR not displayed (<原因>)`。
  `manual` = 峰值用用户覆盖值。**内容状态必须反映真实链路**（用户 2026-10-18 要求
  "OSD 正确显示 content 状态"）：上变换关 / 链上没有滤镜时不能带 `→ HDR10`。
- **接口**：输入 chunky RGB——RGBA8 / **X2BGR10**（10-bit SDR 源，`NvCVImage_Transfer` 落 8-bit）
  / YUV（VSR 直通或软解+hwup：NV12/P010 走共用 `yuv_to_rgba` 内核）；输出
  **`IMGFMT_X2BGR10` + `pl_color_space_hdr10` + `hdr.max_luma = Luminance`**
  （口径对齐 mpv 自己的 `vf_d3d11vpp.c` NVIDIA True HDR 路径），下游全是既有 10-bit 通路。
- **参数**：`luminance` = 面板**物理峰值**（合成器 preferred `maxLuma`，夹到 400–2000，兜底 650）
  ——客户端下发；`saturation/contrast/middle-gray` 默认 100/100/50；
  **`debanding-off` 恒 0（抗色带常开，铁律 6）**，选项只作诊断 A/B。
  **参数变化走 `NvVFX_SetU32` 热更新（零重建）**——实测（`build/scratch/truehdr_setu32.c`，
  1920×1080 合成图、同一引擎只 SetU32 后重跑）：Saturation 100→200 采样色度 0.1627→0.2377、
  MiddleGray 50→80 均值 24.1→38.7 nit、Luminance 400→1000 灰阶带均值 74.6→88.0 nit，且
  **与 destroy+create+Load 同参数的结果逐位相同**。只有 ①帧尺寸变化 ②`DebandingOff` 变化
  （改 pass 图，未实测是否 Load 期决定 → 不假设）③SetU32 失败 才重建引擎（≈25 ms）。
  ⚠ **`VSR_UPHDR_NITS=<400..2000>` = 仅调试的峰值覆盖**（非法值忽略并记 error）；
  默认路径**永远**跟面板能力走（用户 2026-10-18 定："默认按面板能力算，env 只用于调试"）。
  `Luminance` 有**双重作用**：① TrueHDR 的目标峰值参数；② 我们声明给合成器的内容峰值
  （`effectiveContentMeta()` 的 `max_cll`/mastering max）——改它两个后果都会动。
- **TrueHDR 的全部可调参数 = 5 个**（证据 = `strings libnvVFXTrueHDR.so` 里的参数文档串，
  逐条照抄）：`Contrast` 100（0–200）/ `Saturation` 100（0–200）/ `MiddleGray` 50（**10–100**，
  ⚠ 初版我写成 0–100，已修）/ `Luminance` 650（400–2000）/ `DebandingOff` 0（0|1）；
  另有 `BatchSize`（只支持 1，非用户参数）。**没有**质量档/色域/transfer/强度类旋钮
  （VSR 有 `QualityLevel`、VFG 有 `mode`，TrueHDR 没有对应物）。同两串还证实接口假设：
  `SrcImage0` = "RGBA U8 interleaved, CUDA/CPU_PINNED"、`DstImage0` = "RGB10A2 P32 packed,
  HDR10 PQ encoded"。5 个参数全部能用 `vf-command truehdr <name> <value>` 运行期改。
- **⚠ 参数区间是硬约束，且越界是静默的**（2026-10-18 实测，回答"luminance 最低只能 400 吗"）：
  探针 `build/scratch/truehdr_lumrange.c` 逐值新建引擎实测（SetU32 → Load → Run → 读回灰阶
  PQ nit + 字节哈希）：`Luminance` 399/300/200/100/50 与 2500/4000 的
  `NvVFX_SetU32` 全返回 **-7**（invalid parameter value），而 `Load()`/`Run()` 仍返回 0
  → **拒绝不报错、值不生效**；被拒后引擎**保持当前值**（先设 1000 再发 100 → 输出仍 = 1000；
  从未成功设过则停在 SDK 默认 650）。输出展开量随 Luminance 单调上升（灰阶带峰值
  250.6 nit@400 → 318.5@650 → 375.5@1000 → 450.6@2000）→ **400 = SDK 能给的"最小展开"**，
  想更接近原 SDR 只能在下游（mpv tone mapping）调。
  含义：三处同区间夹取（`TrueHdrParams.h`、`vf_truehdr.c` 的 `M_RANGE`、`HdrOutput` 的 clamp）
  是必须的；`engine_params_apply()` 检查 SetU32 返回码（失败 → 重建）是必要防线。
- **成本**：4K 实测 **4.9–5.8 ms/源帧**（探针 CUDA-event 的 2.04 ms 只覆盖引擎 Run，
  不含两次 D2D 拷贝 + 逐帧 `cuMemAlloc/Free`(33 MB) + 同步）。1080p→4K + VFG@60fps
  稳态 22 s **丢帧 0**。
- **声明**：上变换生效时给合成器的内容峰值 = TrueHDR 的 Luminance（`effectiveContentMeta()`）——
  不发它合成器按 10000 nit 口径压，画面会发暗发平（同 §20 的机制）。
- ⚠ **同步 mpv API 死锁（本轮踩到并修，仍然是既有脆弱点）**：`mpv_set_property` 是**同步**的
  （内部等 core 的 dispatch lock）。**软解 + `vo=libmpv` 直渲**时 core 可能卡在
  `get_buffer2_direct → vo_get_image → dr_helper_get_image` **等渲染线程交 DR 图像**；GUI 线程
  若在此同步等 core = 循环等待死锁（gdb 实证；3/3 必现）。修法：① `setHdrTarget` **幂等**
  （值未变不发属性写）；② 首帧 `reevaluate` 从 `afterRendering` **排队到 GUI 线程**
  （渲染回调只留只读探测）。**残余**：目标真的切换时（SDR↔HDR10 运行期切换）仍是同步写，
  若恰有 DR 在等仍可能死锁 → 彻底修法 = 异步属性写（`mpv_command_async` 的 `set`）。
  **软解片源做 HDR 切换相关验收必须覆盖这一格。**
- **UI（2026-10-18 第二轮，用户定的结构）**：`QualityPopup` 里只放 **SDR→HDR10 开关**
  + **Advanced** 图标按钮（`mdi-tune`）；点开 **`TrueHdrAdvancedPopup`**（modal 居中，新文件）
  放参数：Luminance / Saturation / Contrast / MiddleGray 各一条 **slider + Reset**，
  Debanging 是两态开关（无 Reset——两态本身就能回到默认 On）。组件：`ui/components/ParamSliderRow.qml`（单参数行）、
  `ui/components/SegToggle.qml`（Off/On 分段开关）；新 QML 必须登记进
  `src/client/resources.qrc`（QML 走 **qrc**，`view.setSource("qrc:/main.qml")`——漏登记
  的报错是 `Type Xxx unavailable` + `"components": no such directory` 这类）。
  - **滑块实时提交但节流**（`throttleMs` 默认 120 ms，leading+trailing，尾值必发）：
    滤镜侧改参数走 SetU32 热更新（零重建），所以不需要"松手才生效"；节流只为限制
    vf-command 频率。（初版做的是松手才提交，用户 2026-10-18 纠正："做 throttle，
    不要做松手才生效"。）
  - **高级入口是图标按钮**（`mdi-tune` U+F062E，size 16 = 与左侧分段开关同高 32）；
    打开高级弹框时 `uiLayer.closeOtherPopups(trueHdrAdvancedPopup)` **关掉 QualityPopup**。
  - 滑块配色照 `ProgressSlider.qml`（只改配色，不动几何/交互）；高级入口 = `mdi-tune`
    （U+F062E，`docs/icon-cheatsheet.html` 可查）字面字符。弹窗**无背景遮罩**（PopupBase 统一）。
  - ⚠ **初始化赋值不得触发提交**：`ParamSliderRow` 的 `Component.onCompleted` 赋值必须包在
    `_syncing` 里——否则弹窗一实例化就把"显示值"当用户输入写回 VM 并落盘
    （实测：启动即把 `upHdrLuminance` 写成 400）。
  - **Reset 语义 = 回 SDK 默认值**（`TrueHdrParams.h` 的 `*Def`）；**Luminance 例外**：
    回**当前面板峰值**（该参数默认就是"自动跟面板"，用户按 Reset 想要的是自动结果）。
  - **Luminance 覆盖开关**（Auto/Manual）：默认 Auto = 跟面板物理峰值；打开后滑块/Reset
    才可用，且**打开瞬间把滑块种子成当前自动值**（避免一开覆盖画面就跳）。手动值同时是
    声明给合成器的**内容峰值**（`effectiveContentMeta` 的 max_cll），所以 HdrOutput 持有
    该覆盖状态（`hdr::setUpconvertLuminanceOverride`），VM 只是 UI/持久化的拥有者。
  - 持久化键（QSettings）：`upHdrEnabled / upHdrLumOverride / upHdrLuminance /
    upHdrSaturation / upHdrContrast / upHdrMiddleGray / upHdrDebanding`；手改越界值会被
    **夹回区间**而不是整份丢弃。`VSR_UPHDR=on` 降级为 `upHdrEnabled` 的**默认值**
    （无持久化设置时生效），benchmark 不读设置 → 仍只认 env。
  - 区间/默认值的客户端单一来源 = `src/client/TrueHdrParams.h`（数值 = SDK 参数文档串）；
    C 侧镜像在 `vf_truehdr.c` 的选项表——**跨构建目标无法共享头文件，改动两处同步**。
- **未做**：观感门槛（与 `mpv --inverse-tone-mapping` 并排，决定是否默认开）；输入缓冲 wrap
  优化（省一次拷贝）；`--fruc-order pre` 支持；RPC 侧没有 `set-uphdr`（UI 之外只能走
  `vf-command truehdr …` 直发滤镜）；**引擎 `Load()` 失败时 OSD 仍按策略显示 `→ HDR10`**
  （过滤器永久直通 + 一条 WARN，但 OSD 不读过滤器的 `mode=`——要彻底准确需给状态行加
  `mode=failed` 并反馈给 `HdrOutput`，2026-10-18 记）。

### 渲染循环（Qt Quick）

- **纯请求驱动**：mpv update callback（按帧率推送；无活动时完全静默）→ QueuedConnection → 条件 requestRender（`mpv_->update() > 0 || renderRequested_`）→ updatePaintNode → render + reportSwap + vkQueueWaitIdle。**无心跳/无自主循环**——旧文档的"200ms 心跳"模型已证伪。
- `QSG_RENDER_LOOP=basic` 必须（threaded 每帧渲染饿死事件循环）。**绝不无条件 update()/postEvent**（渲染风暴回归教训）。`aboutToBlock` 兜底已删除（`Video.cpp:377` 注释确认）。
- 卡死归因：目录播放概率性卡死根因 = filter 每帧 `get_render_target_size` 持锁等渲染线程 → vo_libmpv.c 无锁读（__atomic relaxed）已修复。
- **Xid 109**（全屏切换 device loss）：根因 = `Video::ensureRenderTarget` 销毁 rtImage 前无 GPU 同步（Vulkan UAF）→ 销毁前无条件 `vkQueueWaitIdle` 修复（`tests/fs_stress.sh` 验证）。

### 启动与会话行为（2026-10-17）

- **命令行输入 = 播放列表（mpv 语义，2026-10-18）**：位置参数与 `--playlist <file|->` 按**出现顺序**登记（`Options::Input`）；`--playlist` 的内容在展开时插入它出现的位置。多条 → 首条 `loadfile`（replace + 播放）+ 其余 `loadfile … append`（`MpvController::appendFile`）。播放列表文件 = m3u/纯文本（跳过空行与 `#` 注释，去行尾 CR），`-` = 从 stdin 读到 **EOF**（ff2mpv wrapper 的 `--playlist=-` 就是这条；交互式终端下别用，会一直等）。给了输入就**不**走 idle 恢复、也不恢复上次列表；展开后为空 → 不载入媒体（对齐 mpv 空列表 → idle）。展开在读入时做（`main.cpp: expandInputs()`），`Options::parse` 保持纯字符串解析、不做 I/O。
- **无参数启动 = idle**：只恢复上次播放列表（`restorePlaylistIdle()` 全 `loadfile … append`，mpv 无 current 条目 → 不解码、不建滤镜链、不 Load 引擎、不做续播 seek），按播放才 `resumeLastPath()` → `playlist-play-index` 载入，mpv 用 watch_later 自动恢复上次进度。命令行给了文件、或显式 `-- --pause=yes` → 立即载入（mpv 语义）。
- 起因：曾有"启动暂停"实现（`pause=yes` 选项 + 启动即载入），但 mpv 精确 seek 会把"关键帧→目标位置"的帧**过完滤镜链再丢掉**（实测 `start=12.302`/关键帧 6.25s → 142 源帧、VFG 570 输出帧白跑、海报晚 1.6s）；日志里那批 `vfg-status` 是丢弃帧不是播放。完整测量与被否方案：`docs/memory/startup-idle-policy.md`。
- idle 时 mpv 的 `pause` 初值为 false → `pause` 观察器按 `updatePlaying(!p && fileLoaded_)` 判定，否则 UI 未载入就显示"播放中"。
- **打开文件夹 = mpv 原生目录展开（2026-10-21）**：`openFiles` **不扫描目录**，目录路径原样交给 mpv `loadfile`（`main.qml` 的 FolderDialog 用 mode 0，与拖入窗口的 DropArea 同一条通路）。mpv `demux_playlist` 按 `--directory-mode`（默认 lazy）与 `directory-filter-types`（默认 video/audio/image/archive/playlist）展开：顶层媒体成条目（文件在前、目录在后，`mp_natural_sort_cmp`）、**子目录作为条目留在列表**、点击时再展开（`playlist-play-index` 后目录条目原地替换为其内容，`PlaylistModel` 前缀 diff 照常处理）。旧实现（mode 3）用 Qt `QDir::entryList` 只扫顶层固定扩展名表 → 子目录/图片/播放列表全丢，与拖放不一致，已删除。证据：测试目录（2 媒体 + 1 子目录 + 1 软链目录 + txt/srt）装载后 mpv 与播放器均 4 条目，点目录条目展开为其中文件。
- **播放列表面板"空白"根因与修法（2026-10-21）**：`PlaylistPanel` 是 `Drawer`（Popup）。关闭时 popup 内容从场景树摘除；**关闭期间**播放列表增删（换片/追加/目录展开）后，ListView 的 delegate 保持旧几何（实测 120→20 条重开后 delegate y 仍是 4326..4872，而 contentHeight=840）→ 视口内没有任何条目 = 面板空白，且**不自愈**（第一次滚轮才跳回）。修法：ListView 放进 `Loader`，`onOpened` 创建 / `onClosed` 销毁（每次打开都是全新布局）；另加 `clampScroll()`（`contentHeight/height/count` 变化时 `Qt.callLater` 夹 contentY，`boundsBehavior: StopAtBounds`）与滚轮 handler 对**全部** wheel 帧 `accept`、写入前 `cancelFlick()`（避免手写 contentY 与 Flickable 的 wheel flick 两套滚动互相打断）。验证：修复前重开截图为空，修复后显示 20 条；120 条滚到底 contentY=4368、末条 120 可见。新 ListView 全新建局 → 每次打开都会重建；`onLoaded` 里 `positionViewAtIndex(currentIndex, ListView.Center)` 把当前播放条目居中（实测 index 61/120 → contentY=2205，前后各 ~16 条可见），无 current（`currentIndex<0`）时回到顶部。
- **两个拖放区（2026-10-21 确认）**：A = 窗口级 `DropArea`（`main.qml`）→ mode 0，**完全重置**播放列表（首条 replace 播放、其余 append 排队）；B = `PlaylistPanel` 的 `DropArea` → mode 1，**追加**（不打断当前播放）。面板打开时 B 在 popup overlay（`QQuickOverlay` 是 contentItem 的子项、创建在 QML 根之后）里叠在 A 之上，Qt 的拖放投递按 `paintOrderChildItems()` **从上到下找第一个接受者**（`qquickdeliveryagent.cpp: deliverDragEvent`），所以两个区不会同时触发。**追加目录**同样走 mpv 原生语义：`loadfile <dir> append` 只留一个目录条目、点击才展开（实测：追加后列表 = `[/tmp/vsr-dir-test]`，点击 → 4 条目并播放首条）。
  菜单侧同语义（2026-10-21）：右键菜单 = "Add files to playlist"（FileDialog mode 1）+ "Add folder to playlist"（FolderDialog mode 1，同一 `folderDialog` 实例靠 `mode` 属性区分标题与 `onAccepted`）；"打开文件/文件夹"仍分开（平台对话框不能同时选文件与目录），但两者与追加共用 `openFiles` 单一入口，目录路径一律交给 mpv。

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
