# VFX TrueHDR（SDR→HDR10）调研 · 2026-10-17

> **分支**：`investigate/vfx-truehdr`。**§1–§7 = 调研与离线测量（不改管线代码）；§8 = 第一轮实现**（2026-10-18 开发，开关默认关）。
> **与既有文档的关系**（不重复，只补增量）：
> - `docs/hdr-pipeline-investigation.md` §1–§2：TrueHDR 的方向/输入假设/参数/静图质量（8-bit 通路背景下的结论）。
> - `docs/vfx-sdk-1.3-investigation.md` §4：SDK 级事实与静图实测（4 张老 JPEG：亮度 ×2.35–2.47、饱和度 −6–15%、debanding 逐位无差异）。
> - `docs/hdr-pipeline-investigation.md` §20–§22：HDR10 上屏已落地（声明 + 静态元数据 + OSD）。
> - **本文补的是当时明确标"未测"的三件事**：① 视频吞吐（当时只有静图，pynvvfx 依赖 torch 无法复跑）；
>   ② 真实视频帧（此前样本是老 JPEG 静图）；③ 在我们管线里的落位与预算核算。
> 关闭该分支前，本条目的结论应合并进 `docs/hdr-pipeline-investigation.md`。

---

## 1. 结论摘要

1. **能力确认可用、代价可忽略**：`TrueHDR` effect 在当前 1.3 库上可直接跑（`CreateEffect/Load/Run` 全通），
   逐帧成本 **720p 0.56 ms / 1080p 0.84 ms / 4K 2.04 ms**（Debanding 开），关掉 debanding 后
   0.13 / 0.31 / 1.24 ms。引擎 `Load()` ≈ **113–123 ms 一次性**，缓冲 1080p 7.9 MB ×2、4K 31.6 MB ×2。
   → 相对现有链路（VSR 4K 16.6 ms + VFG@4K 2.9 ms）**可忽略**，4K24 的 41.7 ms 预算下加 2 ms 毫无压力。
2. **输出格式与文档一致**：`NvCVImage` 实测 `pixelFormat=13`(RGB10A2) / `componentType=11`(P32) /
   `numComponents=4`、chunky；`Luminance` 只抬**高光**（400→650：p90 180.8→210.0 nit、max 250.6→318.5，
   中位不动）。这也顺手闭合了 `docs/vfx-sdk-1.3-investigation.md` §6 遗留的"NVCV RGB10A2 常量未被
   可执行探针验证"——**已在 10-bit 通路工作里验证**（`=13`），本探针再次确认。
3. **质量：方向仍是"上变换"而非"还原"，且真实视频帧的降饱和比静图更明显**：
   线性光域、亮部（>20 nit）饱和度 **catlove −9.1% / heya −29.3%**（静图旧值 −6–15%）；
   中位亮度 ×1.25–1.43、p90 ×1.24–1.47。**内容相关性强**（动画/MMD 类掉得更多）。
4. **落位有两条，且都不需要新通路**：现有 10-bit（RGB10A2）通路 + HDR10 上屏已经覆盖 TrueHDR 的输出
   （PQ/BT.2020 的 RGB10A2）。且**只有 HDR 输出打开时才有意义**——SDR 屏上它等于"先推理再压回 SDR"。
5. **建议：先做零成本替代的 A/B，再决定是否接**。mpv 自带 `--inverse-tone-mapping`（我们已把它作为
   target 选项喂给 gl_video），零代码零推理；若其观感不够，再考虑 TrueHDR（届时**必须**调参：
   `Saturation` 默认 100 会降饱和）。
   ⚠ **`DebandingOff` 不是成本旋钮，必须保持默认 0（开）**：它是"恒定成本、条件收益"的抗色带环节
   （每帧都跑网络，只有检测到色带才改像素），专为"SDR→HDR 上变换放大 8-bit 台阶"而设。旧静图
   "逐位无差异"只说明**那几张图没触发它**，不构成关闭依据——详见 §7 方法学教训。

---

## 2. 事实基线（复述关键项；证据见既有两文档）

| 项 | 值 |
|---|---|
| effect 名 | `TrueHDR`（导出实现 `NvVFXTrueHDRImpl`） |
| 方向 | **SDR → HDR10**（不是"HDR 内容支持"） |
| 输入 | `NVCV_RGBA` + `U8` + chunky；假设 **Rec.709 / D65 / gamma 2.2、全范围**，**不识别源色彩空间** |
| 输出 | `NVCV_RGB10A2` + `NVCV_P32` + chunky，**PQ 编码**、BT.2020；**尺寸必须与输入一致** |
| 参数 | `Contrast` 0–200(100)、`Saturation` 0–200(100)、`MiddleGray` 10–100(50)、`Luminance` 400–2000(650)、`DebandingOff` 0/1(0=开) |
| 底层 | NGX `libnvidia-ngx-truehdr.so.1.2.3`（与驱动级 RTX Video HDR 同源）+ DL-Debander pass |
| 时序 | 纯逐帧、无时序状态、无逐场景自适应 |

⚠ 与 10-bit 通路的区别（别混）：`TrueHDR` 是**换色彩空间**（SDR→PQ/BT.2020）；
10-bit 通路是**保精度**（HDR 保持 HDR）。两者互补、不互替。

---

## 3. 本轮新增测量

探针：`build/scratch/truehdr_probe.c`（自建，dlopen `third_party/nvvfx/lib`，primary CUDA context；
NGX 类引擎用新建 context 会被拒 `Run=-7`）。运行：`./build/scratch/truehdr_probe third_party/nvvfx/lib 20`

### 3.1 吞吐与资源（20 帧/组，CUDA event 计时；合成图与真实帧同值 → 成本与内容无关）

| 分辨率 | 缓冲/张 | `DebandingOff=0` | `DebandingOff=1` | 关掉后倍数 |
|---|---|---|---|---|
| 1280×720 | 3.5 MB | **0.56 ms**（1773 fps） | 0.13 ms（7570 fps） | 4.3× |
| 1920×1080 | 7.9 MB | **0.84 ms**（1191 fps） | 0.31 ms（3252 fps） | 2.7× |
| 3840×2160 | 31.6 MB | **2.04 ms**（489 fps） | 1.24 ms（808 fps） | 1.6× |

- `Load()`：113–123 ms（每种参数组合各建一次；**进链前预热一次即可**，与我们 VSR/VFG 的
  "首次运行预热"同款问题）。
- 逐帧抖动：min/max 与 avg 相差 ≤0.05 ms（4K 0.02–0.07）→ 无尖峰。
- `Luminance` 400 vs 650 对耗时**无影响**（0.84 vs 0.85 ms）。
- **与现有链路预算对比**：4K24 预算 41.7 ms；VSR(1080p→4K) 16.6 ms + VFG@4K 2.9 ms
  + TrueHDR@4K 2.04 ms ≈ **21.5 ms**；1080p 场景更是 +0.84 ms。

### 3.2 输出验证（读回像素 → PQ EOTF → nit）

- 合成图（lum400）：中位 53.9 nit、p90 180.8、max 250.6；lum650：p90 210.0、max 318.5（中位不变）。
- **真实 1080p 帧**（`input/…1080p.webm` 第 5 秒，偏暗帧）：输入假设口径 ~4.0 nit → 输出 6.6–25 nit。
- 采样落盘：`build/scratch/truehdr_ab/*.png`（in / out 成对，out 是 PQ→nit→按目标峰值归一再 gamma2.2 的
  **观看近似**，不是原始 PQ 数据）。

### 3.3 真实视频帧的质量代理（线性光域口径，避开观看映射自身的非线性）

| 素材（720p，第 30 秒 / 第 5 秒） | 中位亮度 | p90 | 亮部(>20nit)饱和度 |
|---|---|---|---|
| catlove（动画，较亮） | 18.4 → 23.0 nit（×1.25） | 110.4 → 137.0 | 0.426 → 0.387（**−9.1%**） |
| heya（MMD/动画） | 90.5 → 129.4 nit（×1.43） | 146.7 → 215.1 | 0.474 → 0.335（**−29.3%**） |

- 方向与旧静图一致（降饱和），**幅度更大且强内容相关**（动画类更明显）。
- ⚠ 口径说明：饱和度在**线性光域**算（输入按 gamma2.2 线性化 ×203 nit；输出由 PQ 反解 nit），
  阈值 >20 nit 只统计亮部；旧静图的 −6–15% 是各自的编码域口径，两者不可直接比大小，只看方向。
- `Saturation` 参数是唯一的口味旋钮（0–200）：要抵消降饱和需要 >100（旧静图记 `Saturation=120` 可拉回）。

---

## 4. 在我们管线里的落位（两个工作点）

前置事实：TrueHDR **不能缩放**（输入=输出尺寸），输入必须是我们内核产出的
"全范围 RGB8"（`yuv_to_rgba` 已处理 limited→full 与 BT.709/601/2020 矩阵 ✓）。

| 方案 | 链路 | TrueHDR 分辨率 | 增量成本 | 备注 |
|---|---|---|---|---|
| **A** | 解码 → VSR(8-bit) → **TrueHDR** → VO | 输出分辨率（4K） | +2.04 ms | TrueHDR 吃放大后的 SDR；输入帧 33 MB |
| **B** | 解码 → **TrueHDR** → VSR(**10-bit**) → VO | 源分辨率（1080p） | +0.84 ms | **复用已落地的 RGB10A2 通路**；AI 超分跑在 PQ 数据上 |

- 两者输出都是 `RGB10A2 + PQ + BT.2020`，正好是 §20/§22 那套 HDR10 上屏（声明 + 静态元数据）能直接吃的数据。
- **B 更省**（源分辨率推理 + 已在跑的 10-bit 通路），但把"AI 超分看到的分布"从 SDR 变成 PQ；
  A 保留"SDR 进超分"的既有行为，成本也只多 1.2 ms。**建议先试 B**（成本/代码量都更小），
  若观感异常再退 A。
- 输入 transfer 的小口径问题：TrueHDR 假设 gamma2.2，而 SDR 源通常是 **BT.1886（≈2.4 带黑位）**
  → 中灰约有 5–8% 偏差，**SDK 无参数可调**（只能自己先做 1886→2.2 的预映射，属可选精修）。
- 触发条件：**只在 HDR 输出启用时**（`VSR_HDR=auto` 且屏在 HDR）才对 SDR 内容跑 TrueHDR；
  SDR 屏上跑它等于"多一次推理再压回 SDR"，纯亏（§2.2 旧结论）。

---

## 5. 与"零成本替代"的对比（未测，列为下一步）

| 替代 | 成本 | 现状 |
|---|---|---|
| mpv `--inverse-tone-mapping`（gl_video/libplacebo 内建） | **0 推理**，纯 shader | 我们的 VO 就是 gl_video，理论上一个选项就能开；**未做 A/B** |
| 只把 SDR 装进 PQ 容器（白点 203 nit，不做亮度扩展） | 0 | 就是 `VSR_HDR=hdr10` 强制模式现在的行为（用户实测：**更暗**，不算 SDR→HDR 上变换） |
| KWin 合成器侧 | — | 合成器**不做** SDR→HDR 上变换（只按 SDR 亮度把 SDR 映到面板） |

→ 因此"要不要自己接 TrueHDR"的真正判据是：**mpv ITM 的观感是否够用**。它零成本、零维护、
且与我们已声明的内容描述天然配合；TrueHDR 的优势目前只有"AI 模型的上变换观感"这一条**未验证**的假设。

---

## 6. 建议与遗留

**建议顺序**：
1. **先做 mpv `--inverse-tone-mapping` 的 A/B**（零代码）：在 SDR 内容 + HDR 屏幕上分别用
   ITM 开/关 + TrueHDR 输出（已有 PNG 对）做目检；若 ITM 够用 → 到此为止，接 TrueHDR 无收益。
2. 若 ITM 观感明显不如 TrueHDR 的 PNG → 按**方案 B** 接（源分辨率 → VSR 10-bit），
   参数起点 `Saturation≥110`、**`DebandingOff` 保持 0（开）**、`Luminance` = 面板峰值（本机 400）。
3. 接之前补测：0 成本替代的量化对比、4K 串联后的显存峰值（33 MB×2 + VSR 缓冲）、长跑稳定性。

**未测/风险（诚实记录）**：
- 视频吞吐**已测**（本文 §3.1），但**串联真实管线后的端到端**（含 VSR/VFG/VO 的排队与显存）未测。
- 质量只做了 2 个真实帧 + 旧静图；**没有全片/多内容类型的统计**，也没有和 ITM 的并排目检。
- `DebandingOff` **不得**因"样本上看不出差别"而关闭（见 §7）：它是为最坏帧（暗部/渐变上的色带）准备的。
- 动画/MMD 类内容降饱和最明显（−29%），若主要观看类型是动画，需重点关注。

**复跑**：
```bash
gcc -O2 -o build/scratch/truehdr_probe build/scratch/truehdr_probe.c \
    -I/opt/cuda/include -L/opt/cuda/lib64 -lcuda -ldl -lm
./build/scratch/truehdr_probe third_party/nvvfx/lib 20                 # 吞吐
./build/scratch/truehdr_probe third_party/nvvfx/lib 10 /tmp/f.rgba W H # 真实帧 + PPM
# 帧提取：ffmpeg -ss 5 -i input/x.webm -frames:v 1 -pix_fmt rgba -f rawvideo /tmp/f.rgba
```
**证据**：`build/scratch/truehdr_probe.c`（探针）、`build/scratch/truehdr_ab/*.png`（in/out 目检图）。


---

## 7. 方法学教训：不假设画面里"没有 X"

本轮我先踩了一个坑：把"4 张静图上 `DebandingOff` 开/关逐位无差异"当成了"可以关掉它省成本"的依据。
**这是错的**，而且错在方法而非数据：

- **视频逐帧、逐镜头变化**：同一部片子里有的镜头干净、有的满屏色带；色带又最常出现在**暗部/平滑渐变**
  ——正是 SDR→HDR 上变换放大得最狠的地方。"样本里没有"≠"不会出现"。
- **条件触发的处理项（抗色带/抖动/去噪/去块）必须常开**：它们的存在就是为了应付最坏帧；
  恒定成本、条件收益是**设计意图**，不是"没触发就等于不需要"。
- **推论**：任何"用样本证明某环节可以省掉/关掉"的论证都不成立；要动这类项，只能靠
  "最坏帧（已知有色带的素材）+ 目标显示条件（真实 HDR 亮度）下的并排结果"，而不是"平均/逐位全同"。
- 同理适用于我们对管线其它环节的判断：不要因为某素材没暴露问题就认为该保护可以省。

→ 该规则已写入 `AGENTS.md` 的工作铁律（第 6 条）。

---

## 8. 第一轮实现方案（TrueHDR 接在 VSR 之后 · 仅方案，未开发）

> 本文档只写方案。开发要在用户点头后另起（可继续用 `investigate/vfx-truehdr` 或新分支）。

### 8.1 范围与已定决策

| 项 | 决定 | 依据 |
|---|---|---|
| 链路位置 | **`vsr` 之后**：post 序为 `hwup → vsr → truehdr → vfg`；RIFE 后端插链尾（`… → vsr → truehdr`） | 用户 2026-10-18 定；VSR 输入保持今天的 SDR 分布不变 |
| `--fruc-order pre` | 本轮**不支持**（该序下"VSR 之后"=链尾=按输出帧计费，等价于最贵方案）→ 记日志、不插入 | §4/§7 的取舍 |
| 运行期开关 | `vf-command truehdr enable\|luminance\|saturation`；由 HDR 策略钩子下发 | 与 `rife fps`/`vsr scale` 同机制 |
| HDR 内容直通 | 过滤器**按帧 transfer 自守卫**（PQ/HLG → 原样透传），不只依赖客户端开关 | 单一事实源，防二次转换 |
| 默认行为 | **默认关**（opt-in `VSR_UPHDR=on`）：收益未证实（默认参数降饱和、ITM 替代未 A/B）、成本吃紧 | §1/§3.3/§5 |
| `DebandingOff` | **恒 0（开）**，不作为成本旋钮；选项保留仅供诊断 A/B | `AGENTS.md` 工作铁律第 6 条 |

### 8.2 开关真值表（Enable = 内容 SDR ∧ opt-in ∧ 策略≠off ∧ 链上有滤镜）

> **2026-10-18 改（用户要求"面板非 HDR 也允许开启 SDR → HDR10"）**：上变换的判据
> 不再含"目标模式 = Hdr10"——**与显示能力解耦**。面板非 HDR 时输出面仍是 SDR
> （不动 swapchain、不下发色彩描述），TrueHDR 产出的 HDR10 帧由 mpv 按其 target
> （`target-trc=auto` = SDR）tone map 回 SDR 写进现有 8-bit 面，即改动前所有 HDR
> 片源走的那条通路。用户 2026-10-18 在 A/B 两个方案里选了 A（不放宽 swapchain/声明）。
> 另加"链上是否真有滤镜"一项：`--fruc-order pre` 不插滤镜，此时上变换与 OSD 标记
> 一起关掉（否则 OSD 会声称 `SDR → HDR10` 而帧没被处理）。

| 内容 | 显示 | policy 目标 | TrueHDR | 说明 |
|---|---|---|---|---|
| SDR | HDR 屏 | Hdr10 | **on** | 上变换 + HDR10 上屏（auto 策略的这一格） |
| SDR | SDR 屏 | Sdr | **on** | 上变换照跑，输出仍 SDR（mpv 把 HDR10 tone map 回 SDR） |
| HDR | HDR 屏 | Hdr10 | off（过滤器自守卫直通） | 走既有 10-bit 直通 |
| HDR | SDR 屏 | Sdr | off | mpv 把 HDR tone map 到 SDR（现状） |
| SDR | 任意 | 任意 | off | `VSR_HDR=off`（诊断逃生舱）或 opt-in 关 |
| SDR | 任意 | 任意 | off | `--fruc-order pre`：链上没有 truehdr（方案 §8.1） |

### 8.3 过滤器实现（`src/mpv/video/filter/vf_truehdr.c`，新）

形状照 `vf_vsr.c`（priv + `NvCVImage` in/out + CUDA stream + `mp_cuda_ctx_ref_*`；lazy init）；
换 context 的坑沿用现有做法（**在旧 context 内**销毁 engine/stream/`cuMemFree`，下帧重建，见 AGENTS「运行中切 hwdec」）。

- **输入**：
  - `mpi->params.color.transfer ∈ {PQ, HLG}` → **直通**（HDR 内容）；
  - 否则 subfmt 是 `RGBA` → 直接喂；是 `X2BGR10`（10-bit 容器的 **SDR** 源）→ 先落到 RGBA8
    （`NvCVImage_Transfer` 或自建 kernel，一次 D2D）。
- **输出**：`IMGFMT_CUDA` + `hw_subfmt = IMGFMT_X2BGR10`；`params.color` 置
  **primaries=BT.2020 / transfer=PQ**（其余继承源：`p_w/p_h`/rotate/light）；`repr` 走
  `mp_image_params_guess_csp`（RGB/FULL）。→ 下游 `vfx_bits_from_imgfmt(X2BGR10)=10`、
  VFG 走 D2D 10-bit、VO 按 HDR 处理，**全部是既有通路**。
- **选项**：`enable`(0) / `luminance`(默认客户端下发的**面板物理峰值**，兜底 650) /
  `saturation`(100) / `contrast`(100) / `middle-gray`(50) / `debanding-off`(**0**，诊断用)。
- **命令**：`.name="truehdr"` + `.command=`（照 `vf_vsr.c:1112/1117` 的注册），支持
  `enable`/`luminance`/`saturation`/`status`（可选 `dump in|out` 供取证）。客户端发
  `vf-command truehdr <cmd> <arg>`（派发名 = 过滤器名，与 vsr/vfg 一致）。
- **失败降级**：`Load()` 失败 → 一次性 WARN + 直通（不每帧重试），与 `vf_vfg` 的 degrade 路径一致。
- **一次性成本**：`Load()` ≈ 113–123 ms → 首帧 hitch。对策：客户端在 FILE_LOADED 重放 enable 时
  即触发 init（首个源帧完成预热）；日志/OSD 标注一次。
- **内存**：本轮分配 in+out 两张 `NvCVImage`（1080p 各 7.9 MB / 4K 各 31.6 MB）；
  「wrap 输入帧缓冲省一张 + 一次拷贝」留到第二轮。
- 复用 `vfx_loader.{c,h}`（共享 dlopen/搜索顺序；避免再踩 AGENTS §8.3 的搜索顺序坑）。

### 8.4 客户端接线

- `PlayerViewModel::vfOption()`：post 序插 `@truehdr:truehdr:enable=no:luminance=<peak>` 于
  `@vsr` 与 `@vfg` 之间；RIFE 分支插链尾。**链串里恒 `enable=no`**（OPT_BOOL 只认 yes/no），实际状态靠运行期命令下发。
- 新钩子：`hdr::ApplyHooks.updateUpconvert(bool on, float peakNit)`，与 `updateMpvTarget`
  **同一批调用点**（mode 变化 / display 变化 / 内容变化 → `syncTargets`）；判定在 `HdrOutput` 内
  （`applied==Hdr10 && !contentHdr && optIn`）→ 单一事实源。
- 下发路径：复用现有 `mpv_->commandAsync({"vf-command", "truehdr", …})`（`PlayerViewModel.cpp:1649` 附近）。
- **FILE_LOADED 重放**：链重建会把所有 vf 选项打回链串默认 → 必须把 TrueHDR 的
  enable/luminance 并入 `syncVfOptions` 的重放列表（与 `rife fps`/`vsr scale` 同款）。
- OSD（可选）：`hdr::statusText()` 在上变换生效时追加 `· upconvert`。

### 8.5 验收（开发后按此测）

1. **真值表**：SDR/HDR 内容 × HDR/SDR 屏 × opt-in 开关 → 逐格核对 `truehdr status` 与 OSD。
2. **运行期**：播放中切屏 HDR 开关、换片（SDR↔HDR）、软↔硬解切换（CUDA context 重建）→
   不崩、无校验错误、engine 重建成功。
3. **格式**：日志确认输出 `X2BGR10 + transfer=PQ`；`dump` 落盘 / 截图目检（需 HDR 屏）。
4. **性能**：1080p→4K + VFG 场景 on/off 的渲染 fps 与丢帧（预估 +2.04 ms/源帧 @4K）。
5. **观感门槛**（决定是否默认开）：与 `mpv --inverse-tone-mapping`（零成本）并排；
   不合格就保持默认关。

### 8.6 本轮不做（第二轮的候选）

pre 序（TrueHDR 在 VSR 之前）与"VSR 吃 PQ"的验证；参数 UI/自动调参；`DebandingOff=1`；
ITM 与 TrueHDR 自动择优；输入缓冲 wrap 优化；RIFE 后端的 k× 成本优化。

### 8.7 改动清单（预估）

| 文件 | 改动 |
|---|---|
| `src/mpv/video/filter/vf_truehdr.c`（新） | 过滤器本体（选项/命令/直通守卫/引擎生命周期） |
| `src/mpv/meson.build` | 注册 `vf_truehdr.c`（复用 `vfx_loader`） |
| `src/client/PlayerViewModel.{h,cpp}` | `vfOption()` 插链；`syncVfOptions` 重放；命令下发 helper |
| `src/client/HdrOutput.{h,cpp}` | `updateUpconvert` 钩子 + 判定 +（可选）OSD 标记 |
| `src/client/main.cpp` | 钩子接线 |
| `AGENTS.md` + 本文件 | 实现后的状态与证据 |

---

## 8.8 第一轮实现记录（2026-10-18，已开发 + 已验收）

### 8.8.1 实际改动（与 8.7 的差异）

| 文件 | 实际改动 |
|---|---|
| `src/mpv/video/filter/vf_truehdr.c`（新，~950 行） | 过滤器本体；引擎段（`NvVFX_CreateEffect("TrueHDR")` + 两张 `NvCVImage`）**内联**实现（不另开 `truehdr_proc.c`：只有本滤镜用，且比 vsr/vfg 的引擎薄） |
| `src/mpv/video/filter/vfx_loader.{c,h}` | 无接口改动；库列表加 `libnvidia-ngx-truehdr.so` + `libnvVFXTrueHDR.so`（都带存在性守卫，缺失只打一行说明）；世代日志加 `TrueHDR` 标记 |
| `src/mpv/meson.build`、`filters/user_filters.{c,h}` | 注册 `vf_truehdr` |
| `src/client/PlayerViewModel.{h,cpp}` | 链串插 `@truehdr:truehdr:enable=no:luminance=<peak>`（vsr 与 vfg 之间；RIFE 链尾；**pre 序不插**并记一行）；`setUpconvert()` + `syncVfOptions()` 重放 |
| `src/client/HdrOutput.{h,cpp}` | `ApplyHooks.updateUpconvert`；auto 策略新增"SDR 内容 × HDR 屏 × opt-in → Hdr10"格；上变换判定与显示能力解耦（§8.2，2026-10-18）+ `setUpconvertInChain()`（pre 序不插滤镜时同关）；`effectiveContentMeta()`（上变换时内容峰值 = Luminance）；OSD 内容标记 `content SDR → HDR10(Nnit[, manual])`；**首帧 reevaluate 改为排队到 GUI 线程** |
| `src/client/MpvController.{h,cpp}` | `setHdrTarget` **幂等化**（见 8.8.4 的死锁） |
| `src/client/main.cpp` | 钩子接线 + `truehdr-status:` 转发 stderr |

**两处与方案不同的判断**（都有依据）：
1. 输出标注改为 **mpv 自己的参考实现口径**：`pl_color_space_hdr10` + `hdr.max_luma = Luminance`
   （`third_party/mpv/video/filter/vf_d3d11vpp.c:410` —— mpv 的 NVIDIA True HDR 路径同样是
   "标 HDR10 + 猜 max_luma=1000"；我们**知道**真实值，就如实填）。HDR 内容判定也用
   libplacebo 的 `pl_color_space_is_hdr()`（同文件 `:151` 的判据），不再手写 transfer 比较。
2. `dump in|out` **未做**：改成 status 行里的**输出 PQ 采样**（读回 32 点 → PQ EOTF → nit 的
   中位/峰值）。对"输出确实是 PQ 编码"这件事，数值比 PNG 目检更硬（HDR 截图本身不可信，§6）。

### 8.8.2 验收结果（`VSR_HDR=hdr10` 强制 + `VSR_UPHDR=on` 为可达条件；本机面板当前在 SDR 档）

| 项 | 结果 |
|---|---|
| 真值表 SDR×HDR屏(强制)×opt-in | **on**：`hdr upconvert: on（luminance=400）`、`engine ready 3840x2160 (Load 25 ms)`、`VO: cuda[x2bgr10]`、`vfg: reconfig bits=8 → bits=10` ✔ |
| 真值表 SDR×SDR屏(auto)×opt-in | 改动前 = **off**；**2026-10-18 改为 on**（用户要求"面板非 HDR 也允许开启"）：`hdr upconvert: on（输出 SDR；面板峰值 400 nit …）`、`truehdr-status: mode=active`、`dropped 0`，swapchain/声明不动（`hdr verify: swapchain=SDR == 期望 SDR`）✔ |
| OSD 内容标记 | 面板 SDR：`HDR  SDR · content SDR → HDR10(400nit)`；HDR10 输出（`VSR_HDR=hdr10` 强制验证）：`HDR10 · BT.2020/PQ · display 200nit · content SDR → HDR10(400nit) · (forced)`；用户覆盖峰值时带 `, manual`（`SDR → HDR10(1209nit, manual)`）✔ |
| `--fruc-order pre` × opt-in | 链上无 truehdr → 上变换关、OSD **不声称** `SDR → HDR10`（`SDR · content SDR`），stderr 一条 `truehdr-status` 都没有 ✔ |
| 真值表 HDR×HDR屏(强制)×opt-in | 客户端策略收敛到 **off**；过滤器同时**自守卫直通**（`HDR 内容（transfer=12 primaries=6）— 直通`），`VO: cuda[p010]` 走既有 10-bit 直通 ✔ |
| 四种输入形态 | RGBA8（VSR 后的 8-bit 源）/ **X2BGR10**（10-bit SDR 源经 VSR，D2D+`NvCVImage_Transfer` 落 8-bit）/ NV12（VSR 直通 + 硬解）/ P010（软解 + hwup）——全部 active，无越界、无错误 ✔ |
| 运行期：软↔硬解切换 | 三次上下文切换各重建引擎一次（`engine ready` ×3，Load 22–25 ms），状态行全程 active，无校验错误 ✔ |
| 运行期：换片 SDR↔HDR | SDR→HDR：upconvert→off + 自守卫直通；HDR→SDR：重放 `enable=1` 并重建引擎 ✔ |
| 性能（1080p→4K，VSR+VFG@60fps 输出，30fps 源，22s 稳态） | off：drop=0，VSR 8.4–8.9 ms；on：**drop=0，TrueHDR 4.9–5.8 ms/源帧**，VSR 不变 ✔ |
| 输出数值 | `peak=222…399 nit`（luminance=400）、`med=5…21 nit`；合成器声明含 `max_cll 400` ✔ |
| 观感门槛（§8.5 #5，与 mpv ITM 并排） | **未做**（需面板切 HDR + 目检，留给用户） |

⚠ **成本口径与 §3.1 的差异**：探针 CUDA-event 计时 2.04 ms@4K 只覆盖引擎 `Run`；管线内实测
4.9–5.8 ms 还包含两次 D2D 拷贝、逐帧 `cuMemAlloc/cuMemFree`（4K 输出缓冲 33 MB，与
`vf_vsr` 同款"拥有权移交"模式）与流同步延迟。**做预算用 4.9–5.8 ms**（4K24 的 41.7 ms 预算下
VSR 16.6 + VFG 2.9 + TrueHDR ~5.5 ≈ 25 ms，仍有余量）。

### 8.8.3 踩过的坑（都已修，值得记）

1. **`OPT_BOOL` 只认 `yes`/`no`**（不认 `0`/`1`）。链串里写 `enable=0` 会让**整条 `vf` 选项解析失败**
   → 链上所有滤镜都不生效（现象：`VO: 720p cuda[nv12]` 直通 + A/V 不同步），而 mpv 只在
   stderr 打一行解析错误。链串用 `enable=no`；`vf-command` 侧仍接受 `0|1|on|off|yes|no`。
2. **10-bit 中转图必须随引擎一起作废**：`in10_img` 按当时的帧尺寸分配，换片（尺寸变化）后
   复用会 D2D 越界。现在 `engine_destroy()` 一并 `Dealloc`。

### 8.8.4 ⚠ **同步 mpv API 在"core 等 DR 图像"时会死锁**（本轮新发现的既有脆弱点）

- **现象**：加了 truehdr 节点后，**软解**片源（`get_buffer2_direct` 直渲路径生效）启动即**整体卡死**
  （GUI/事件线程/RPC 全无响应，stderr 停在首帧探测），3/3 必现；基线（无该节点）3/3 正常。
- **根因**（gdb 全线程栈，`ptrace_scope=1` 下用 `gdb --args` 启动才能抓）：
  `Thread1(GUI) mpv_set_property("target-trc") → mp_dispatch_lock` **等 core**；
  core 卡在 `avcodec_send_packet → get_buffer2_direct → vo_get_image → dr_helper_get_image`
  **等渲染线程交 DR 图像**；而渲染/事件循环正是被阻塞的 GUI 线程 → **循环等待**。
  事件线程同时卡在 `mpv_get_property("pause")`（`onFileLoadedFromEventThread`）。
- **触发条件**：任何**同步** mpv API 调用（属性读写）落在"core 正在等直渲图像"的窗口里。
  软解 + `vo=libmpv` 才有 DR；硬解不会走这条路（所以此前所有 HDR 切换验收都没撞上）。
- **本轮修法**（最小、对齐既有做法）：
  ① `MpvController::setHdrTarget` **幂等**（值未变不发属性写）——首帧/显示能力/元数据变化会
     反复用相同值调用，判等后绝大多数退化为 no-op，死锁窗口消失（实测 3/3 恢复）；
  ② `installCapabilityProbe` 的首帧 `reevaluate("first-frame")` 从 `afterRendering`（渲染回调）
     **排队到 GUI 线程**再做（渲染回调只留只读探测）。
- **仍存的残余风险（未修，留给下一轮）**：目标**真的需要切换**时（如 SDR→HDR10 运行期自动切换）
  仍是同步属性写；若此时恰有 DR 图像在等，仍可能死锁。彻底的修法是改走**异步**属性写
  （`mpv_command_async` 的 `set target-trc …`，mpv 会拷贝命令）或把目标更新搬到事件线程。
  本次没在 SDR 面板上构造出"播放中切换"，故未验证；**面板切 HDR 后需专门测这一格**。

### 8.8.5 使用方式（本轮）

```bash
# 默认关：行为与今天逐字节一致（链上多一个直通节点）
./build/src/client/vsr-player <file>

# 打开 SDR→HDR10 上变换（面板 HDR 与否都能跑：HDR 面板顺带切 HDR10 上屏，
# 非 HDR 面板输出保持 SDR，末段由 mpv 把 HDR10 tone map 回 SDR）
VSR_UPHDR=on ./build/src/client/vsr-player <file>

# 诊断/机制验收（不需要面板支持 HDR；强制 HDR10 上屏）
VSR_HDR=hdr10 VSR_UPHDR=on ./build/src/client/vsr-player <file>
```
运行期（RPC）：`{"command":["command","vf-command","truehdr","enable","1"]}` /
`…,"luminance","400"` / `saturation|contrast|middle-gray` /（诊断）`debanding-off`。
状态行（stderr）：`truehdr-status: mode=active cost=…ms fps=… luminance=400 peak=…nit med=…nit`。
OSD 的 `HDR` 行：上变换生效时内容段显示 `content SDR → HDR10(400nit[, manual])`
（真 HDR 片源不变：`content 758nit`）。

### 8.8.6 可调参数 = 5 个（SDK 权威清单，2026-10-18 补）

证据 = `strings third_party/nvvfx/lib/libnvVFXTrueHDR.so` 里的参数文档串（逐条照抄，不是
从文档/记忆推的）。**TrueHDR 只有这些**，没有质量档/色域/transfer/强度类旋钮（VSR 有
`QualityLevel`、VFG 有 `mode`，TrueHDR 没有对应物）：

| 参数 | SDK 默认 | SDK 范围 | 我们 |
|---|---|---|---|
| `Contrast` | 100 | 0–200 | 选项 + `vf-command truehdr contrast` |
| `Saturation` | 100 | 0–200 | 选项 + `… saturation` |
| `MiddleGray` | 50 | **10–100** | 选项 + `… middle-gray`（初版写成 0–100，已修） |
| `Luminance` | 650 | 400–2000 | 客户端按面板峰值下发 + `… luminance` + env 覆盖 |
| `DebandingOff` | 0 | 0 或 1 | 恒 0（抗色带常开，铁律 6）；`… debanding-off` 仅诊断 |

另有 `BatchSize`（"only 1 supported"，非用户参数）。同两串还证实接口假设：
`SrcImage0` = "RGBA (U8, Interleaved), NVCV_CUDA 或 NVCV_CPU_PINNED"、
`DstImage0` = "NVCV_RGB10A2 (P32 packed, Interleaved), HDR10 PQ encoded"。

**`Luminance` 的取值来源**（`HdrOutput.cpp: upconvertLuminance()`）：
1. 默认 = 面板**物理峰值**（合成器 surface preferred 的 `maxLuma`，与 OSD `display Xnit` 同源），
   每次策略重算重读（面板 HDR 开关/换屏/内容变化都会重取）→ 夹到 400–2000；读不到退 650。
2. `VSR_UPHDR_NITS=<400..2000>` = **仅调试的覆盖**（非法值记 error 并按面板峰值；用户 2026-10-18
   定："默认按面板能力算，env 只用于调试"）。
3. 运行期 `vf-command truehdr luminance <400..2000>` 可临时改，但**不持久**——下一次策略重算
   会用面板值覆盖回去。

⚠ `Luminance` 有**双重作用**：① TrueHDR 的目标峰值参数；② 我们声明给合成器的内容峰值
（`effectiveContentMeta()` 的 `max_cll` / mastering max）。所以调它两个后果一起变——A/B 时
一次只动一个参数才能归因。

实测（`VSR_UPHDR_NITS=1000`，强制 HDR10）：日志 `hdr upconvert: 目标峰值被 VSR_UPHDR_NITS 覆盖为
1000 nit` + 过滤器 `max_luma=1000`（输出标注与声明都跟随）；非法值 `100` → `hdr: VSR_UPHDR_NITS=100
非法（SDK 只接受 400–2000 nit）→ 忽略，按面板峰值`，随后仍落到 400。
`middle-gray=5` 现在被选项解析拒绝（`The middle-gray option must be >= 10: 5`）。

**⚠ `[400..2000]` 是硬约束，且越界是静默的（2026-10-18 实测，回答用户"最低只能 400 吗"）**

证据 = 新探针 `build/scratch/truehdr_lumrange.c`（每个取值新建引擎：SetU32 全参数 → Load →
Run → 读回灰阶带的 PQ nit + 原始字节哈希）：

```
lum    setrc    load   run    gray_mean    gray_max     hash
50     -7       0      0      83.82        318.47       0x2287eecf96bcf8ec   ← 与 650 行逐位相同
100    -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
200    -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
300    -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
399    -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
400    0        0      0      75.57        250.58       0x70e40d4452f0bb0a
650    0        0      0      83.82        318.47       0x2287eecf96bcf8ec   ← SDK 默认
1000   0        0      0      89.92        375.53       0x630ef6939ef152c3
2000   0        0      0      97.14        450.55       0xff3cc1162185429c
2500   -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
4000   -7       0      0      83.82        318.47       0x2287eecf96bcf8ec
```

- `NvVFX_SetU32(..., "Luminance", v)` 对 v<400 与 v>2000 都返回 **-7**（invalid parameter value），
  `Load()`/`Run()` 仍返回 0 → **拒绝是静默的**；被拒的取值不生效。
- 被拒后引擎用的是**当前值不变**（不是回默认）：先 SetU32→1000（rc=0）再发 100（rc=-7），
  输出哈希仍 = 1000 那次（`0x630ef6939ef152c3`）。上表 <400 那几行哈希 = 650 行，是因为那些
  引擎**从未成功设过** Luminance → 停在 SDK 默认 650。
- 输出展开量随 Luminance 单调上升（灰阶带峰值 250.6 nit@400 → 318.5@650 → 375.5@1000 → 450.6@2000）
  → **400 就是 SDK 能给的"最小展开"**；想更"透明"（更接近原 SDR）在 TrueHDR 侧做不到，
  只能在下游（mpv 的 tone mapping target/peak）调。
- 对我们的意义：① 三处夹取（`TrueHdrParams.h` 的 `kLuminanceMin/Max`、`vf_truehdr.c` 的
  `M_RANGE(400,2000)`、`HdrOutput` 的 clamp）都是必须的——同一区间镜像三份；② 面板 SDR 档
  合成器报 `max_luma=200` → 自动值必落在 400 下限；③ 选项目志/`vf-command` 层已挡越界，
  所以 `engine_create()` 里不检查 SetU32 返回值**当前无害**（越界会静默跑 650），
  但 `engine_params_apply()` 检查（失败 → 重建）是必要防线。

### 8.8.7 参数 UI（2026-10-18 第二轮）

用户定的结构：**`QualityPopup` 只放"SDR→HDR10 开关 + Advanced… 按钮"**，参数在独立
弹框 **`TrueHdrAdvancedPopup.qml`**（modal 居中）里，每个参数 **slider + Reset**。

| 界面元素 | 行为 |
|---|---|
| `QualityPopup` 的 Off/On 开关 | 总开关（= 策略 opt-in）。打开后还要满足 `HDR10 上屏 ∧ 内容 SDR` 才真正生效——下面一行小字直说"Running / Enabled, idle (needs HDR display + SDR content)" |
| `Advanced…` | 打开 `TrueHdrAdvancedPopup`（不关 QualityPopup：modal 关掉后回到原处） |
| Luminance 行 | **Auto/Manual 覆盖开关**（关 = 自动跟面板，滑块/Reset 变灰）；打开瞬间把滑块**种子**成当前自动值（不跳变）；Reset = 回**面板当前值** |
| Saturation / Contrast / MiddleGray | slider + Reset（Reset = SDK 默认 100 / 100 / 50） |
| Debanging | Off/On（默认 On = SDK 默认）+ 说明"off 是诊断，快 1.6–4.3×" |

- **滑块实时提交但节流**（`ParamSliderRow.throttleMs` 默认 120 ms，leading edge 立即发 +
  拖动中每 120 ms 一次 + 尾值必发）：滤镜侧改参数走 `NvVFX_SetU32` **热更新（零重建）**，
  所以不需要"松手才生效"（初版做的是松手才提交，用户 2026-10-18 纠正）。
- **高级入口 = 图标按钮**（`mdi-tune` U+F062E；字形先在字体 cmap 里查过存在）；打开高级
  弹框时 `uiLayer.closeOtherPopups(trueHdrAdvancedPopup)` **关掉 QualityPopup**。
- 滑块**只调配色**、照 `ProgressSlider.qml`（轨道 `#44ffffff` / 已填充 `#e0e0e0` /
  滑块 `#ffffff` 14×14 r7）——项目里没有的视觉语言不引入（用户 2026-10-18 立为铁律 7）。
- **Reset = SDK 默认值**（`TrueHdrParams.h` 的 `*Def`；SDK 无 preset，默认值就是唯一"预设"）。
- **Luminance 手动值同时是给合成器的内容峰值**（`effectiveContentMeta` 的 max_cll）→
  覆盖状态由 `HdrOutput` 持有（`hdr::setUpconvertLuminanceOverride`），VM 负责 UI/持久化。
- 持久化键：`upHdrEnabled / upHdrLumOverride / upHdrLuminance / upHdrSaturation /
  upHdrContrast / upHdrMiddleGray / upHdrDebanding`（手改越界 → 夹回区间）。
  `VSR_UPHDR=on` 降级为默认值；`VSR_UPHDR_NITS` 仍是**调试**覆盖（优先级最高，
  在自动值之上但 UI 里看不到）。
- 新文件：`src/client/ui/TrueHdrAdvancedPopup.qml`、
  `src/client/ui/components/ParamSliderRow.qml`、`src/client/ui/components/SegToggle.qml`、
  `src/client/TrueHdrParams.h`；**必须登记进 `src/client/resources.qrc`**（QML 走 qrc；
  漏登记 = `Type Xxx unavailable` + `"components": no such directory`）。

**验收（无 UI 目检可做的部分）**：把测试配置（`XDG_CONFIG_HOME` 重定向，不动用户配置）
写成 `upHdrEnabled=true, upHdrLumOverride=true, upHdrLuminance=1000, upHdrSaturation=137,
upHdrContrast=88, upHdrMiddleGray=63, upHdrDebanding=false` → 日志逐条核对：

```
hdr upconvert: 用户开关 → on（重算目标模式）
hdr upconvert: 目标峰值 → 用户覆盖（1000 nit）
truehdr: 参数 = on，saturation=137 contrast=88 middle-gray=63 debanding=off(diagnostic)，峰值=用户覆盖
syncVfOptions: truehdr enable=0 lum=1000 sat=137 contrast=88 mg=63 deband=0
已声明 HDR10（… + max_cll 1000 / max_fall 0）
truehdr: engine ready 3840x2160 (Load 24 ms) luminance=1000 contrast=88 saturation=137 middle-gray=63 debanding=OFF(diagnostic)
truehdr: 3840x2160 … → X2BGR10 HDR10 (primaries=6 transfer=12 max_luma=1000)
```

即：**UI 设置 → 持久化 → 过滤器引擎参数 → 输出标注 →（关键）合成器声明的内容峰值**
整条链路一致。`VSR_UPHDR=on`（无持久化键）→ 开；不设 → 关（filter 全程 passthrough）
两条也各测了一遍。QML 侧无 `TypeError/Cannot assign/Binding loop` 告警。
**参数滑块的观感/手感需要用户目检**（我这边只能证明链路与数值）。


### 8.8.8 参数热更新（SetU32）—— 让"拖动滑块"零卡顿（2026-10-18）

**问题**：UI 滑块要实时生效就得频繁改参数，而原实现"任何参数变化 → destroy+create+Load"
（≈25 ms + 图像重分配）在拖动中会反复打嗝。

**测量**（探针 `build/scratch/truehdr_setu32.c`，1920×1080 合成图，**同一引擎**只
`NvVFX_SetU32` 后重跑，与"destroy+create+Load 同参数"对照）：

| 操作（Load 之后） | 采样指标 | 结果 |
|---|---|---|
| 基线 Saturation=100 MiddleGray=50 Luminance=400 | 色度 0.1627 / 均值 24.4 nit | — |
| `Saturation` 100→200 | 色度 **0.2377** | 生效 |
| `MiddleGray` 50→80 | 均值 **38.7 nit** | 生效 |
| `Luminance` 400→1000（灰阶带） | 均值 74.6→**88.0 nit** | 生效 |
| 对照组：重新 Load（同最终参数） | 色度 0.2537 / 均值 38.7 nit | **与 SetU32 路径逐位相同** |

→ **Load 之后 SetU32 生效，且与重建等价**。实现改为：`engine_params_apply()` 只对**变化**的
参数发 SetU32；只有 ①帧尺寸变化 ②`DebandingOff` 变化（改 pass 图，**没测**是否 Load 期决定
→ 不重建不假设）③SetU32 返回非 0 才走重建。

**管线内实测**（mpv CLI + `--input-ipc-server` 发 4 条 `vf-command`）：

```
truehdr: params 100/100/50/400 -> 100/150/50/400 (SetU32, 无重建)   ← saturation 150
truehdr: params 100/150/50/400 -> 120/150/50/400 (SetU32, 无重建)   ← contrast 120
truehdr: params 120/150/50/400 -> 120/150/70/400 (SetU32, 无重建)   ← middle-gray 70
truehdr: params 120/150/70/400 -> 120/150/70/900 (SetU32, 无重建)   ← luminance 900
engine ready 次数 = 1（全程零重建）
```

### 8.8.9 面板非 HDR 也允许开启 + OSD 内容状态（2026-10-18 第三轮）

**用户要求**：①"面板非 hdr 也允许开启 sdr -> hdr10"；②"OSD 正确显示 content 状态，
如果是 sdr -> hdr10 内容，需要有对应信息"。

① 的语义有两解，问过用户后**选 A**：上变换**与显示能力解耦**（面板 SDR 时也跑），
输出端保持现状——**不放宽** swapchain/色彩空间声明；TrueHDR 产出的 HDR10 帧由 mpv 按其
target（`target-trc=auto` = SDR）tone map 回 SDR 写进现有 8-bit 面（= 改动前所有 HDR
片源的通路）。方案 B（面板 SDR 也强制 HDR10 上屏 = 把诊断用的 `VSR_HDR=hdr10` 接到开关上）
未采纳。

实现（`HdrOutput.cpp`）：
- 上变换真值表去掉"目标模式 = Hdr10"一项：`Enable = 内容 SDR ∧ opt-in ∧ 策略≠off ∧ 链上有滤镜`；
- 新增 `hdr::setUpconvertInChain(bool)`（`PlayerViewModel::initTrueHdr` 按 `frucOrderPre()`
  设，须在 `installPolicy` 前）——`--fruc-order pre` 的链里没有 truehdr 滤镜，此时上变换与
  OSD 标记一起关掉（否则 OSD 会声称 `SDR → HDR10` 而帧没被处理）；
- auto 策略里"SDR 内容 × opt-in"那一格加 `st.upconvertInChain` 条件；
- 日志 `hdr upconvert: …` 增打 **`输出 <模式>`**（面板 SDR 时一眼看出"只跑上变换、不上屏 HDR10"）。

② 的 OSD 形态（`statusText()`；`upconvert(Nnit)` 改成内容标记）：
- HDR10 输出 + 上变换：`HDR10 · BT.2020/PQ · display 400nit · content SDR → HDR10(400nit[, manual])`
- **面板非 HDR** + 上变换：`SDR · content SDR → HDR10(400nit[, manual])`
- 真 HDR 片源：`content 758nit`（形态不变）；未上屏：`SDR · content HDR not displayed (<原因>)`
- `manual` = 峰值用用户覆盖值（否则跟面板）

**实测验收**（面板当时在 SDR 档：`kscreen-doctor` HDR/WCG 都 disabled；surface preferred
`tf=2 bt709 gamut_area=0.1120 max=200 → SDR volume`）：

| 场景 | 证据 |
|---|---|
| `VSR_UPHDR=on`，面板 SDR，SDR 内容（普通运行） | `hdr upconvert: on（输出 SDR；面板峰值 400 nit，生效峰值 1209 nit = 用户覆盖…）`；`truehdr-status: mode=active cost=2.0ms fps=30 luminance=1209`；`hdr verify: swapchain=SDR == 期望 SDR ✔`（**不动输出**）；`hdr status: SDR · content SDR → HDR10(1209nit, manual)` |
| 同上（benchmark，读 OSD 文本） | `HDR  SDR · content SDR → HDR10(400nit)`，`帧数 rendered 2646 dropped 0` |
| `VSR_HDR=hdr10` 强制（验证 HDR10 分支文案） | `HDR10 · BT.2020/PQ · display 200nit · content SDR → HDR10(400nit) · (forced)`，`truehdr-status: mode=active`，dropped 0 |
| `--fruc-order pre` × opt-in | 链上无 truehdr（stderr 一条 `truehdr-status` 都没有），OSD = `SDR · content SDR`（**不声称**上变换）✔ |
| 面板状态 | 全程未动（HDR/WCG 仍 disabled）✔ |

⚠ **已知不准的一格（未做）**：TrueHDR 引擎 `Load()` 失败（VFX 库缺失/损坏）时过滤器
永久直通（一次性 WARN），而 OSD 仍按**策略意图**显示 `→ HDR10`——策略是单一事实源，
OSD 不读过滤器的 `mode=`。要彻底准确需给状态行加 `mode=failed` 并反馈给 `HdrOutput`。

**第四轮补充（同日）：OSD 的"输出"行带实际输出模式：**

用户原话："OSD 缺少实际输出的信息，HDR10/SDR"；追问后明确"**实际输出合并进输出行**，
目前 HDR 行信息完备"。实现：

- `HdrOutput` 新增 `outputText()`（`HDR10`/`SDR`）+ `PolicyState.actualSwapchain/haveActual`：
  值来自**真实探测**（首帧 `installSwapchainVerification` 的 `probeSwapchain().current`、
  换屏重探）与每次 `applyMode` 成功后的落地值，**不是策略意图**——Qt 静默回退时两者
  不同，以探测值为准（日志 `hdr output: 实际输出 = SDR`）；探测/变化时补发
  `statusChanged` 让 OSD 跟着刷新。
- `main.cpp` 的 `statusChanged` 钩子同时推 `viewModel.setHdrOutput(hdr::outputText())`
  （GUI 线程写、事件线程 `osdTextString()` 读 → 与 `hdrStatus_` 同款加锁）。
- `osdTextString()` 的 `Output` 行：`2560×1440 148.1fps · SDR`（HDR 行不动，分工 =
  HDR 行讲内容侧、Output 行讲显示输出侧）。

实测（`--benchmark` 读 `osd-msg1`，面板当时 SDR 档）：

```
默认（无 VSR_UPHDR）           输出  2560×1440 1610.0fps · SDR    HDR  SDR · content SDR
VSR_UPHDR=on                   输出  2560×1440  560.0fps · SDR    HDR  SDR · content SDR → HDR10(400nit)
VSR_HDR=hdr10 VSR_UPHDR=on     输出  2560×1440  530.0fps · HDR10  HDR  HDR10 · BT.2020/PQ · display 200nit · content SDR → HDR10(400nit) · (forced)
```

普通运行日志另证实取值来自探测：`hdr output: 实际输出 = SDR`（在 `hdr verify:
swapchain=SDR == 期望 SDR ✔` 之前一行，即首帧探测喂的）。
