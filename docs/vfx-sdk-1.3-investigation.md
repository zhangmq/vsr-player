---
name: vfx-sdk-1.3-investigation
description: NVIDIA VFX SDK 1.3（nvidia-vfx 0.2.0.0）升级与新增 effect 调研：VideoFrameGeneration 新 AI 插帧引擎、TrueHDR、10-bit 通路，含实测数据与接入要点
date: 2026-10-15
branch: feat/vfx-sdk-1.3
base: fc1d1b4（2026-10-16 rebase 到 master；原 base f794198）
---

# VFX SDK 1.3（nvidia-vfx 0.2.0.0）调研记录

> **范围**：VFX SDK 1.3 的版本事实、新增 effect（`VideoFrameGeneration` / `TrueHDR`）、VSR 更新（10-bit 通路）、以及为评估它们所做的工作与实测数据。
> **不含**：本机显示器/HDR 输出管线相关话题（另行讨论）。
> **状态**：调研与独立探针验证完成；**尚未写任何播放器侧代码**（`vf_vfg` 未实现），分支上无跟踪文件改动。

## 0. 结论摘要

1. **1.3 在 Linux 上新增两个可用 effect**：`VideoFrameGeneration`（AI 插帧）与 `TrueHDR`（SDR→HDR10）。SDK 文档里其余 filter（AI Green Screen、Background Blur、Upscale、Webcam Denoise、Video Relighting）**不在此 Linux 包内**。
2. **VFG 是全新的 AI 插帧引擎**——不是项目曾调研的 NVOFA 硬件光流路线（自建 warp/blend，repeat 90.8%、有运动对成功率 9.7%），也不是停更的 `libNvOFFRUC`。二进制证据显示是 NGX/FGX 的 AI 模型（FP8/CUTLASS），**在 Linux + Blackwell 上可加载、可推理、且是真插值**。
3. **速度是 VFG 的最大价值**：1080p MEDIUM **2.57 ms/对**、4K MEDIUM **3.55 ms/对**，且**成本几乎与分辨率无关**（内部固定工作分辨率）。对照 RIFE 4.26 v2（**均为播放器内实测**，见 §3.5 修订）：1080p **16.3 ms/对**、4K **195–307 ms/对** → 1080p 快 ~6×，**4K 是 ~85×**（4K 的 RIFE 成本**超线性**，旧的外推值 70–90 ms 严重低估）。
4. **画质与 RIFE 同档，不是压倒性胜出**：heya_720p 2999 对同口径下 VFG MEDIUM 运动对成功率 97.1% / 伪影桶 dev≥4 4.9%，RIFE 4.26 v2 为 98.1% / 6.6%。
5. **TrueHDR 可用但收益有限**：转换全自动（5 个参数、默认中性），默认参数下平均饱和度反而降 6–15%；`DebandingOff` 开关在测试样本上**逐位无差异**。
6. **建议路径**：① 接入 `vf_vfg`（默认 MEDIUM，4K 可切 HIGH）→ ② VSR/VFG 的 **RGB10A2 10-bit 通路**（1.3 新增，项目当前是 8-bit）→ ③ TrueHDR 视是否做 HDR 输出管线而定。

## 1. 版本与环境事实

| 项 | 值 |
|---|---|
| wheel | `nvidia-vfx 0.2.0.0`（内部 **VFX SDK 1.3.0.0**），`manylinux_2_27/2_28_x86_64` |
| 大小 / 校验 | 443,100,523 字节；sha256 `0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8`（与 NVIDIA 官方索引一致） |
| 来源 | `https://pypi.nvidia.com/nvidia-vfx/`（pypi.org 仅占位 sdist） |
| 驱动要求 | Linux `570.190+` / `580.82+` / `590.44+`；本机 **615.78.08**（AGENTS.md 记的 610.57.04 已过时） |
| GPU | RTX 5060 Ti（Blackwell sm_120） |
| VFG 硬件要求 | **Ada / Blackwell**（Linux 额外支持 Hopper）→ 40 系及以上 |
| TrueHDR 驱动要求 | Linux 570+（官方性能表含 T4/Turing → 门槛最低） |
| 许可 | 不变：SLA 2025.05.05 §8.5 禁止再分发；`install.sh` 从官方索引取 wheel 的既有路径继续适用 |

**effect 可用性（实测 `NvVFX_CreateEffect`）**：

| 选择器 | 结果 |
|---|---|
| `VideoSuperRes` / `VideoFrameGeneration` / `TrueHDR` | **0（可用）** |
| `Upscale` / `Denoise` / `Deblur` / `GreenScreen` / `AIGreenScreen` / `BackgroundBlur` / `WebcamDenoise` / `Relighting` / `VideoRelighting` / `ArtifactReduction` | **-2（`NVCV_ERR_UNIMPLEMENTED`）** |

wheel 内也**只有三个 effect 库**：`libnvVFXVideoSuperRes.so`、`libnvVFXVideoFrameGeneration.so`（218 MB，最大）、`libnvVFXTrueHDR.so`。denoise/deblur 不是独立 effect——它们是 **VSR 的 8–15 档模式**（项目已接入）。

## 2. 已完成的工作（实施）

- **分支**：`feat/vfx-sdk-1.3`（base master `f794198`）；**无跟踪文件改动**（VFX 库在 gitignore 的 `third_party/`）。
- **wheel 获取与校验**：aria2 多连接分段下载会把大成员写坏（sha256 不符、unzip CRC 报错）→ 改为**按成员整段下载 + CRC32 校验**（`fetch_members.py` / `watch_members.py`），38/38 文件成员校验通过。
- **运行时安装布局**（`install-vfx-1.3.sh`）：
  - `third_party/nvvfx/lib/` ← 1.3 全部库（578 MB，旧版为 1.1 GB）
  - `third_party/nvidia_vfx.libs/` ← external `libcudart-256e6409.so.12.9.79`（**RPATH 需要**，见 §8）
  - 无版本软链：`libnppc.so`/`libnppial.so`/…/`libcudnn.so`/`libnvidia-ngx-vsr.so`/`libnvidia-ngx-truehdr.so`
- **验证手段**：独立 C 探针 + Python 统计器（都直接 dlopen 指定目录的库，绕开播放器的库搜索顺序），见 §9。

## 3. VideoFrameGeneration（新 AI 插帧引擎）

### 3.1 它是新引擎（证据链）

- **官方文档**：VFG 是 "AI-powered temporal interpolation filter"，Low/Medium/High 三档模型，自带自动场景切换检测（[1.3 文档](https://docs.nvidia.com/maxine/vfx/1.3.0/Filters/VideoFrameGeneration.html)）。
- **二进制**（`libnvVFXVideoFrameGeneration.so`，218 MB）：
  - 符号：`FGX::VideoFrameGenerationModel`、`FGX_Small/Medium/Large::Model`
  - kernel：`model_*_conv1..8`、`model_*_attn1/attn2`、`model_small_*_fp8`、`model_*_uncertainty_p95`、`model_*_warp_coarse_kernel`、`FGX_DIM32_*_CUTLASS_M128`、`FP8 NaN detected`
  - 资源：`_binary_fgx_large_dim32_no_refinenet_2026_q1_fp16_bin_*`、`_binary_kernel_downscale_kernel_cubin_*`、`FGX_LARGE_FLOW_RESOLUTION`
  - **否证旧引擎**：全库无 `NvOFFRUC` / `nvOpticalFlow` / `NvOF` / `nvcuvid` / `nvinfer` 符号
- **与旧路线对比**：`libNvOFFRUC`（Optical Flow SDK 的 FRUC 库，sm_75/sm_80 cubin 无 PTX → Blackwell 不可用，SDK 停在 5.0 / 2023-02）；项目自建 NVOFA+后处理（`tests/fruc/fruc_of_sm120.cu`）。
- FP8 内核 + "Ada/Blackwell only" 的硬件要求自洽（FP8 需 Ada 之后的 Tensor Core）。

### 3.2 正确性：是真插值，不是复制/混合

判据设计（合成"已知运动方块"：src0 方块中心 x=350、src1 中心 x=550，t=0.5 期望中点 450）：
**单峰落中点 = 真插值；双峰（350+550）= 两帧 50/50 混合；单峰在端点 = 复制。**

| 分辨率 | LOW | MEDIUM | HIGH |
|---|---|---|---|
| 720p | 质心 437.6，单峰 | 452.0，单峰 | 448.4，单峰 |
| 1080p | 444.1，单峰 | 452.4，单峰 | 435.2，单峰 |
| 4K | 440.2，单峰 | 449.6，单峰 | 449.2，单峰 |

三档 × 三分辨率**全部通过**；中点列能量 ~17,400 vs 两端 ~108（背景）→ 确实合成了中间帧。

### 3.3 性能（纯推理，ms/对）

| 分辨率 | LOW | MEDIUM | HIGH |
|---|---|---|---|
| 1280×720 | 0.56 | 2.67 | 19.90 |
| 1920×1080 | **0.66** | **2.57** | 19.05 |
| 3840×2160 | **1.60** | **3.55** | 21.21 |

**关键结论：成本几乎与分辨率无关**（1080p→4K 像素 ×4，MEDIUM 仅 2.57→3.55 ms；HIGH 恒为 ~20 ms）。对应内部固定工作分辨率（`dim32` / `FLOW_RESOLUTION` / `downscale_kernel`）。

实时预算判定（每生成帧）：

| 场景 | 预算 | LOW | MEDIUM | HIGH |
|---|---|---|---|---|
| 1080p 30→60fps | 33 ms | ✅ | ✅ | ✅ |
| 1080p 60→120fps | 16.7 ms | ✅ | ✅ | ❌（19.1） |
| 4K 30→60fps | 33 ms | ✅ | ✅ | ✅ |
| 4K 60→120fps | 16.7 ms | ✅ | ✅ | ❌（21.2） |

### 3.4 真实素材质量（heya_720p，3000 帧 / 2999 对，与 `tests/fruc/run_rife.py` 同口径）

口径：`dp=|m−prev|`、`dn=|m−cur|`、`ds=|cur−prev|`（0–255 MAE）、`dev=dp+dn−ds`、repeat 阈值 `min(dp,dn)<0.8`。

| 引擎 | REPEAT | 有运动对 repeat | 运动对成功率 | dev≥4（伪影风险） | 剪切点直通 |
|---|---|---|---|---|---|
| VFG LOW | 7.4% | 2.6% | 97.4% | 5.8% | 2/70 |
| VFG MEDIUM | 8.9% | 2.9% | 97.1% | 4.9% | 40/70 |
| VFG HIGH | 9.5% | 3.2% | 96.8% | 5.0% | 51/70 |
| RIFE 4.26 v2 | 6.6% | 1.9% | 98.1% | 6.6% | 工装未实现 |

**读法提醒（重要）**：
- **repeat 低 ≠ 好**：跨跳变"插值"会产出既不 repeat 也不对的垃圾帧，反而压低 repeat 率。必须**分开看运动对、伪影桶、剪切点行为**。
- RIFE 那行的"剪切点直通"是**离线工装的缺陷**（只跑裸模型，未实现场景切换检测）；`vf_rife.c` 本身有场景切换直通。质量数据有效，该列无效。
- **RIFE 侧计时无效**：ORT 动态 shape（`['batch',7,'height','width']`）触发 ORT 插入 **33 个 memcpy 节点**（ORT 自带告警），`sess.run` 达 281.7 ms/对 → 计时不可用；RIFE 的真实速度用项目自己的 TRT 引擎量（§3.5）。
- VFG 的三档在**同一段素材上是同一档次**，不存在"碾压 RIFE"；VFG 的优势在**速度**与**自带剪切检测**。

### 3.5 RIFE 侧速度基准（来自项目自身，非本轮新测）

| 来源 | 数值 |
|---|---|
| 播放器 `fruc-status cost=`（480p，goose，scale 3） | 3.7 ms/对（`build/fruc-probe/v-light.log`） |
| 播放器 `fruc-status cost=`（1080p29.97，Jellyfish，scale auto 1.33×） | **16.3 ms/对**（`build/fruc-probe/v-heavy.log`） |
| 播放器 `fruc-status cost=`（**2160p**，Fallout，scale auto=直通） | **195–307 ms/对**（`build/fruc-probe/v-4k.log`，上屏仅 3–4 fps） |
| `docs/memory/hardware-fruc-status.md`（864×480 口径） | 13.4 ms/对 |
| `docs/memory/fruc-research-conclusion.md`（4.25 时代，1080p） | 22 ms |

**修订（2026-10-16）**：早期版本这里按像素线性外推给出「4K ≈70–90 ms」，与实测不符。实测 1080p→4K 像素 ×4、成本 ×12–19（**超线性**，疑与 TRT workspace/带宽有关），故 4K 的相对结论必须按实测值给：**VFG 4K MEDIUM 3.55 ms 对 RIFE 195–307 ms，是 ~85× 而非「量级差距」这种含糊说法**。上表前三行均已换成播放器内实测（`VSR_LOG_VIDEO=1` + `fruc-status` 同源日志），不再依赖外推。

### 3.6 接入要点

- **selector**：`"VideoFrameGeneration"`；图像绑定 `SrcImage0` / `SrcImage1` / `DstImage0`，流 `CudaStream`。
- **参数**（`NvVFX_SetU32`）：`Mode`（0/1/2 = LOW/MEDIUM/HIGH）、`FrameMultiplier`、`FrameIndex`、`Timestep`、`ShotChange`、`AutomaticShotChangeDetectionEnabled`、`InputWidth`、`InputHeight`。**batch size 只支持 1**。
- **时间戳语义**：multiplier 模式 t = i/M（与 `vf_rife.c` 整数倍率的"固定 tval i/k"方案天然对齐）；timestep 模式给任意 t（对应非整数倍率的绝对网格路径）。
- ⚠️ **文档不一致**：官方 1.3 文档写 `FrameMultiplier` 范围 **2–8**，ComfyUI 节点允许 2–16 → 实现以官方为准。
- **场景切换**：SDK 自带检测，直通行为 = **复制当前帧**（与 `vf_rife.c` 的"复制时间较近端点"略不同）；实测直通率随档位上升（LOW 2/70 → MEDIUM 40/70 → HIGH 51/70）→ 接入应选 **MEDIUM 及以上**，或显式传 `ShotChange`。
- **分辨率限制风险**：库内有 `FGX dim32 native no-refine unsupported geometry: %ux%u ratio %.4f` → 存在几何/宽高比限制，超宽/竖屏素材需实测。
- **4K 高频损失**（已测，单裁剪样本）：MEDIUM 的 mid/blend 高频比 **0.918**、HIGH **0.956**（1080p 同口径为 1.15）→ 4K 有可测软化，但目检（1:1 裁剪）未见可见软化、颗粒保留 → 4K 建议 HIGH。

## 4. TrueHDR（SDR→HDR10）

**性质**：AI SDR→HDR10 转换，输出 **BT.2020 + PQ、打包 RGB10A2**；转换全自动，**无逐帧/逐场景自适应**。

| 参数 | 范围 | 默认 | 性质 |
|---|---|---|---|
| `Luminance` | 400–2000 | 650 | **校准项**（目标屏峰值亮度） |
| `Contrast` | 0–200 | 100 | 口味（默认中性） |
| `Saturation` | 0–200 | 100 | 口味（默认中性） |
| `MiddleGray` | 10–100 | 50 | 口味 |
| `DebandingOff` | 0/1 | 0（开） | 开关 |

**输入假设固定**：全范围 **Rec.709 / D65 / gamma 2.2** 的 RGB8——**不识别源色彩空间**，BT.601/limited range 等必须先自行转换。

**静图实测**（4 张 2002–2004 年老 JPEG，840×1200 / 690×1000，含 4:2:0 与 4:4:4；素材为私密内容，未纳入仓库）：

| 指标 | 原图 | TrueHDR（lum400） | 说明 |
|---|---|---|---|
| 中位亮度 | 20.6 / 27.8 / 42.6 / 31.9 nit | 48.3 / 65.8 / 105.5 / 76.1 nit | **×2.35–2.47** |
| 高光 p99 | 93–99 nit | 341–389 nit | **×3.7–3.9** |
| 平均饱和度 | 0.298–0.365 | 0.254–0.328 | **默认参数下降低 6–15%**；`Saturation=120` 才拉回 |
| 色带代理（行内等值连跑） | 1.72–3.69 | 1.80–3.81 | 基本不变 |
| 高频（噪声/锐度） | 4.82–8.22 | 4.61–8.03 | 基本不变 → **没有放大噪声** |
| 色域越界（BT.2020→709） | — | **1.4–7.7%** | SDR 屏显示不出来的部分 |

- **`debanding` 开关逐位比对：`max|Δ| = 0`、非零像素 0.000%**（4 张图全部）→ DL 去色带 pass 在该样本上无任何作用。参数通路本身正常（改 `Saturation`/`Contrast` 输出会变）。
- **`Luminance` 影响很大**：650 → p99 500–580 nit、SDR 回映射裁剪 9–17%；400 → p99 340–390 nit、裁剪 1.6–3.4%。
- **目检偏好**：默认档（650/100/100/50）在测试样本中观感最好——注意其高光裁剪率与色带代理**指标更差**，说明这类指标不能替代目检。

## 5. VSR 与 10-bit 通路

- **VSR 对静图天然生效**（纯 2D 逐帧算子，无时序状态；VFG 才是需要两帧的时序算子）。实测 840×1200 → 4×（3360×4800，16 MP）**4.8–7.4 ms/张**；1.3 下 720p→1440p 单帧 **2.80 ms**。
- **10-bit（RGB10A2）是 1.3 新增能力**（VSR 与 VFG 均支持）。项目当前用 `NVCV_RGBA`/`NVCV_U8`（8-bit，见 `vsr_proc.c:212`），而 `yuv_to_rgba` 已处理 P010/P016 10-bit 源 → **10-bit 源被压成 8-bit 再进 AI**，这是一条明确的画质升级点，且不需要动渲染管线。
- **4× 超分 vs bicubic**（同区域 1:1 裁剪）：高频 **+31–37%**；回环 PSNR **35.4–36.7 dB** vs bicubic **40.4–45.1 dB**（改动幅度更大 = 确实生成了细节）。
- **"细节涂抹"归因**：ULTRA 档的压缩伪影抑制（官方文档：强度越高越 "soften fine texture"）。同区域档位对比的高频：

| 图 | bicubic | ULTRA | **HIGHBITRATE_ULTRA** | MEDIUM | LOW |
|---|---|---|---|---|---|
| 老图 A（4:2:0） | 2.21 | 2.62 | 2.61 | 2.37 | 2.48 |
| 老图 B（4:2:0） | 2.45 | 2.81 | **2.84** | 2.55 | 2.67 |
| 老图 C（4:4:4） | 4.22 | 5.14 | **5.37** | 4.97 | 5.16 |
| 老图 D（4:4:4） | 4.74 | 5.73 | **6.39** | 5.61 | 5.86 |

→ `HIGHBITRATE_ULTRA` 纹理保留最多（跳过伪影抑制），`MEDIUM` 最平；代价是可能把块噪一起保留。**判模型请用 4:4:4 素材**——4:2:0 源的色度细节在压缩时就已丢失，任何档位都救不回。

## 6. 未做 / 待办

1. **`vf_vfg` 未实现**（本轮只做调研 + 独立探针验证）。
2. **库落位未解决**（见 §8.3）——要真正在播放器里跑 VFG，必须先让 1.3 落到播放器实际使用的路径。
3. **10-bit 通路未实现**（VSR/VFG 的 RGB10A2）。
4. **4K 画质只做了单裁剪样本目检**，未做全片核对。
5. **TrueHDR 是否接入未定**（取决于是否做 HDR 输出管线）。
6. **`vsr_proc.c` 的 TRT10 预加载可清理**：1.3 不再捆 `libnvinfer`，该预加载退化为无害的失败路径。

## 7. 遗留问题（诚实记录）

1. **旧库备份 `lib.1.2.bak` 被误删**：`ab-vsr.sh` 的 `restore()` 分支把旧目录当临时目录 `rm -rf` 掉了（含 672 MB `libnvinfer`）→ 回滚需重新拉 `nvidia-vfx==0.1.0.1` wheel。
2. **首轮 VSR A/B（旧 284.4 fps vs 新 278.8 fps）作废**：`vsr_proc.c` 的库搜索顺序是 `$HOME/.local/lib/vsr-player/` **优先于** dev 树，而本机装了旧 VFX → 两次跑的都是旧库。
3. **RIFE 侧 ORT 工装计时不可用**（33 个 memcpy 节点，偏慢 ~35×）。若要继续用 ORT 做实验，需冻结动态 shape（`add_free_dimension_override_by_name`）或改用 TRT。

## 8. 关键实现细节与坑

### 8.1 RPATH 与 cudart 摆位
新库 `RPATH = $ORIGIN/../../nvidia_vfx.libs`，且 NEEDED 是 auditwheel 的 mangled 名 **`libcudart-256e6409.so.12.9.79`**（不是 `libcudart.so.12`）。因此目录层级必须照 wheel 摆：
`third_party/nvvfx/lib/` + `third_party/nvidia_vfx.libs/`。安装态（`~/.local/lib/vsr-player/`）同理需要 `~/.local/nvidia_vfx.libs/`，否则 dlopen 失败。

### 8.2 无版本软链
`vsr_proc.c` 按**无版本名** dlopen（`libnppc.so`、`libcudnn.so`、`libnvidia-ngx-vsr.so`…），wheel 里是带版本名 → 必须补软链。

### 8.3 库搜索顺序坑（最重要）
`load_nvvfx_libraries()` 的顺序是 `VSR_INSTALL_LIBDIR` → `$HOME/.local/lib/vsr-player/` → dev 树（仅当可执行文件路径含 `/build/`）→ `build/lib`，取**第一个含 `libNVCVImage.so`** 的目录。**本机装了旧 VFX 时，从 build 树跑播放器用的仍是 `~/.local` 的库**，`third_party/nvvfx/lib` 被遮蔽。要生效必须跑 `install.sh` 或调整优先级。

### 8.4 TRT10 冲突源消失
新 wheel **不再捆绑 `libnvinfer*`**（旧版 1.1 GB 里 672 MB 是它）→ 历史上"VFX 的 TRT10 与系统 TRT11 ABI 冲突"的根源不再存在。

## 9. 证据与产物

**全部在 `third_party/.vfx-dl/`（未跟踪，可整目录删除）**：

| 文件 | 用途 |
|---|---|
| `vfx13_probe.c` / `vfx13_probe` | 独立探针：dlopen 指定目录的库，跑 VSR/VFG，含"已知运动方块"真插值判据、分辨率/档位扫描 |
| `vfg_fruc.c` / `vfg_fruc` | VFG 离线统计器：stdin rgb24 → dp/dn/ds/dev + 分桶 + repeat + 高频 + PPM dump |
| `enum_effects.c` / `enum_effects` | effect 可用性枚举（只调 `CreateEffect`） |
| `truehdr_stills.py` | TrueHDR 静图测试（PQ→SDR 映射、nit 口径、指标、PNG 输出） |
| `vsr4x.py` / `vsr4x_modes.py` | VSR 4× 静图超分与档位对比（含 bicubic 基线、1:1 裁切） |
| `mk_cmp.py` | 同区域 1:1 并排对比图生成 |
| `run_rife426.py` / `run_rife426_diag.py` | RIFE 4.26 v2 fp16 ONNX 同口径统计 / 分阶段诊断 |
| `fetch_members.py` / `watch_members.py` / `install-vfx-1.3.sh` | wheel 按成员下载校验 / 就绪落地 / 库切换（含回滚说明） |
| `vfg_m0.txt` `vfg_m1.txt` `vfg_m2.txt` | VFG 三档 heya 全量统计存档 |
| `fruc_trt.log` | 播放器 TRT RIFE 基准（cost=8.0ms） |

**复现命令**（示意，需按实际路径）：

```bash
# VFG 性能 + 真插值判据
third_party/.vfx-dl/vfx13_probe <libdir> vfg 20 1920 1080 1
# VFG 全片统计
ffmpeg -v error -i input/heya_720p.webm -frames:v 3000 -f rawvideo -pix_fmt rgb24 - \
  | third_party/.vfx-dl/vfg_fruc <libdir> 1280 720 1
# effect 枚举
third_party/.vfx-dl/enum_effects <libdir>
```

## 10. 建议的 AGENTS.md 修订点

1. **过时结论**："Blackwell 上官方硬件插帧唯一路径 = NVOFA 出 flow + 自建 warp/blend" —— 1.3 的 `VideoFrameGeneration` 是新的 AI 引擎，Linux/Blackwell 实测可用。
2. **驱动版本**：610.57.04 → **615.78.08**。
3. **新增坑**：dev 树与 `~/.local/lib/vsr-player/` 的库搜索优先级（§8.3）。
4. **新增事实**：VFX 1.3 不再捆 `libnvinfer`（TRT10 冲突源消失）；新增 RGB10A2 10-bit 通路；`vsr_proc.c` 的 TRT10 预加载可清理。
5. **插帧性能语境**：VFG 1080p/4K MEDIUM 仅 2.6–3.6 ms/对 → 插帧侧的过载压力大幅缓解（与跨节点性能自适应的设计讨论相关）。

## 11. 接入决策（2026-10-16，实施中）

> 本节是**决策记录**，区别于 §3–§5 的调研数据。标「待实测」的项尚未验证。

### 11.1 后端定位

- **VFG = 默认后端**；**RIFE 冻结**，降级为**源码编译可选路径**（后续不再做主要投入）。
- 预编译 tarball（standard / full）**只带 VFG**：不收集 `libnvinfer*`、不构建也不打包 engine。要 RIFE 走 `-Dfruc_backend=rife` 源码构建。
- 有意接受的代价：**插帧 GPU 下限从 Ampere 抬到 Ada/Blackwell**（`README_zh.md:66/145` 现承诺 Ampere+，需同步）。换来彻底去掉 TensorRT 依赖：91MB engine、引擎构建脚本、TRT 版本绑定。

### 11.2 两套语义（编译期分支）

| | VFG | RIFE |
|---|---|---|
| 目标语义 | **整数倍率** `off/2/3/4` | 目标帧率 `off/40/48/60`（不变） |
| 理由 | SDK `FrameMultiplier` 只支持 2–8，非整数倍率需 `Timestep` 模式（**未验证**）→ 不做目标帧率 | 冻结；不动其绝对网格路径 → 零回归 |
| 编译宏 | `FRUC_BACKEND_VFG`（默认） | `FRUC_BACKEND_RIFE` |
| meson | `-Dfruc_backend=vfg`（默认） | `-Dfruc_backend=rife` |

界面标签随分支由 C++ 产出（`frucModel` 从 QML 上移到 `PlayerViewModel` 的 `Q_PROPERTY`），**不 fork QML**。倍率语义下失去了「目标 ≈ 源就直通」的保护（原 `vf_rife.c:529` 的 `src_fps >= opts->fps*0.95`）——×2 对 60fps 源即 120fps 输出，需在 UI 侧按显示刷新率提示（**待做**）。

### 11.3 链序 = 后置（`hwup → vsr → vfg → VO`）

保留 `--fruc-order=pre|post` 开关，默认 **post**。

**本轮补充的关键事实：4K 内容下 VSR 恒为直通。**

- 代码：`vf_vsr.c:258` `need <= 1.0 → return 1.0`（`auto` 比的是**渲染目标**面积而非显示器），`:622` `effective_scale==1 && denoise==-1 → passthrough`——**VSR 从不下采样**。
- 实测：`build/fruc-probe/v-4k.log` 20 次、`4k60.log` **210 次** `vsr-status` 全部为 `mode=passthrough scale=1.00`，一次 active 都没有。

由此：

- **4K 档：前置 == 后置**（成本与画质完全相同——VFG 两序看到的都是同一批 4K 帧）→ 链序收益 **0**。这一档的收益 **100% 来自换引擎**（实测 RIFE 195–307 ms/对 → VFG MEDIUM 3.55ms ⇒ ~85×）。
- **链序收益只出现在「VSR 真在放大」的档位**：1080p29.97 + `scale=auto`(1.33×)、k=2、VSR 实测 8.4ms → 前置 `77 + 504 = 581 ms/s`，后置 `84 + 252 = 336 ms/s`（**省 42%**）。
- 原先担心的陷阱「后置把 VFG 推到 4K 输入 → 被迫 HIGH(21.2ms) → 吃掉 VSR 的节省」**不成立**：它要求「VSR 在跑 ∧ VFG 输入是 4K ∧ 必须 HIGH」三者同时成立，而**原生 4K 恰恰是 VSR 直通的场景**（那里 VSR 不收费，HIGH 完全付得起）；而「VSR 放大到 4K」时 VFG 吃的是 **VSR 合成**的高分，§3.6 的 `0.918` 是在**原生 4K** 上测的，不可直接迁移。
- 例外：**开了 1:1 降噪（denoise）时 4K 档 VSR 不再直通**（`:622` 要求 `denoise == -1`），此时后置仍能把它从 60 帧/s 降到源帧率。
- 附带：原生 4K 下前后置的 VFG 缓冲都是 4K（3×4K RGBA ≈ 100MB），只有「放大到 4K」时后置才多占，且可考虑直接绑定 VSR 输出缓冲做零拷贝（**本轮不做**）。

### 11.4 Mode：用户可调质量档，`auto` 恒 MEDIUM（**2026-10-16 修订**）

**原设计（已废弃）**：`mode=auto` 按**输入高度**解析——≥2160 → HIGH，否则 MEDIUM。**废弃理由（播放器内实测）**：后置链序下「非 4K 源经 VSR 放大到 4K」（1080p + `vsr:scale=2`，即 4K 屏全屏的 auto 档）会让 VFG 输入高度正好踩中 2160 → 自动选 HIGH，而这一档 VSR 正在跑、整链预算最紧：

| 4K 输出（1080p × scale=2、×2、0 丢帧） | VFG cost | 帧时间 中位/p90/最大 |
|---|---|---|
| HIGH（原 auto 取值） | **13.2ms** | 13.99 / **24.06** / **27.18** ms |
| MEDIUM | **3.8ms** | 16.20 / 16.48 / 18.62 ms |

HIGH 在 4K 上贵 **3.5×**、帧时间尾部 p90 从 16.5 恶化到 24.1ms（另一次同配置运行累积 9 个丢帧，运行间可变）；而 MEDIUM 在**原生 4K** 素材上 `cost=2.7ms` / 48fps / 0 丢帧已足够，HIGH 换来的画质差仅为 4K mid/blend 高频 0.918 vs 0.956（**目检未见可见软化**）。更根本的理由：**按分辨率猜档位属于「节点内依据局部信息做全局决策」**，与 PR #19 移除的 RIFE 性能自动降级同源。

**新设计**：
- `mode` 暴露为**用户可调质量档** `low / medium / high`，**默认 medium**（界面一排 / `--fruc-quality` / RPC `set-fruc-quality` / 持久化）。
- 客户端**总是发显式 `mode`**。**（2026-10-16 决定）`auto` 已从 `vf_vfg` 彻底移除**——不再作为兜底语义存在，滤镜只接受 `low|medium|high`（非法值含 `auto` 一律拒绝），默认值改为显式 medium。理由：不要自动档，档位只由用户显式选择。
- 4K 输出下三档实测成本：**LOW 1.2ms / MEDIUM 2.1ms / HIGH 11.0ms**（每输出帧）。
- HIGH 未被移除，只是不再被自动选中（`mode=high` / `vf-command vfg mode high` 显式启用）。

### 11.5 实现要点

- 抽 `vfx_loader.{c,h}` 共用 VFX 库定位/dlopen（VSR / VFG / 未来 TrueHDR 三家同源）——**只抽加载层，不抽帧调度层**（RIFE 冻结，重构它无收益）。
- 后置意味着 `vf_vsr` 的动态 `effective_scale` 会传播到 VFG 的输入尺寸 → 必须实现**重配路径**（改 `InputWidth/InputHeight` 后重新 `Load`）。这条路径也是「后续一行切回前置」的前提。
- 暂**不做** VSR 输出缓冲的零拷贝绑定（后置专属优化，会把链序焊死）。
- 已核实：`scale=auto` 不依赖滤镜位于链尾——`stream_info` 挂在 output_chain filter（`f_output_chain.c:702`）上，而用户滤镜是它的子节点（`:259`），`mp_filter_find_stream_info` 沿父链向上查找（`filter.c:682`，`:810` 确认 `f->in->parent` 是父 filter）。

### 11.6 待实测（P3 及后续）

1. ~~pre vs post 的实际上屏率对照~~ → **已完成（2026-10-16）**。结论：**两者服务质量相同**（帧时间 p90 16.48 vs 16.51ms、最大 18.62 vs 18.66ms、均 0 丢帧），差异全在成本结构——后置的 VSR 只跑 30 帧/s、前置要跑 60 帧/s（1080p × `scale=2` 4K 输出实测：后置 VSR≈99 ms/s + VFG≈228；前置 VFG≈108 + VSR≈390）→ **后置净省约 170 ms/s**。链序保持**后置**，`--fruc-order` 留作可切换（输出分辨率越高后置优势越小：VSR 省下的是固定 `k` 倍帧数，而 VFG 单帧成本随分辨率涨 1080p 1.8ms→4K 3.8ms）。
2. ~~`mode=auto` 的 HIGH 门槛（2160）是否合适~~ → **已废弃，见 §11.4 修订**（改为用户可调质量档、默认 medium、`auto` 恒 medium）。
3. **k≥3** 时每对需 (k−1) 次 `Run` 的成本与 `FrameIndex` 用法（探针只测过 k=2 / `FrameIndex=1`）——**成本已实测**（480p k=3 ≈1.3–1.7ms/输出帧、k=4 ≈1.5ms），**内容正确性**仍只由 PTS/帧数证明，未做像素级验证。
4. VFG **几何限制**（超宽/竖屏/非 16:9/奇数尺寸）边界与越界行为。
5. 10-bit（RGB10A2）通路：FFmpeg CUDA hwcontext 是否接受 `X2RGB10` 作 sw_format、NVCV `RGB10A2` 的真实字节序（`truehdr_fmt` 探针枚举到 pf=12 就静默退出，**该常量至今未被可执行探针验证**）。
6. TrueHDR：Qt 6.12 已有 `QRhiSwapChain::Format::HDR10` 与 `libQt6Quick` 的 `_qt_sg_hdr_format`，但**公开 API 不明**（QtQuick 头文件无 colorSpace/HDR 公开接口）→ 需 spike 确认激活方式。
7. **场景切换语义**：`AutomaticShotChangeDetectionEnabled=1` 是唯一检测源，但引擎内部是否真把中间帧替换为当前帧、判定阈值如何，**无法从外部观测**（无回读信号）；与 `vf_rife`「复制较近端点」的差异是按 SDK 语义写的，非实测。
8. **VFG 成本非"与分辨率无关"**：调研文档 §3.3 的结论在玩家内偏乐观——实测 1080p 输入 1.8–2.0ms、1440p 4.7ms、4K 3.8ms（含共享流等待，非纯推理），故链序优势会随输出分辨率收窄。

## 12. 关联

- `docs/trt-engine-investigation.md`：RIFE/TRT 引擎调查（同族记录）
- `docs/memory/fruc-research-conclusion.md` / `hardware-fruc-status.md`：NVOFA 旧路线结论（本轮结论**不改写**它们，只是新增第三条路）
- `docs/memory/vfx-trt-symlink-rename.md`：TRT10/11 冲突历史（1.3 后不再适用）
- `docs/third-party-setup.md`：VFX 获取与安装（升级到 1.3 后需同步 cudart 摆位说明）
