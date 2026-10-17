---
title: HDR 管线调研：VFX TrueHDR（SDR→HDR）、VFX 10-bit 通路、mpv/Qt 侧 HDR 现状
date: 2026-10-17
status: 调研完成 → **方案 A 已于 2026-10-17 实施并验证**（10-bit 内部通路；B/C 未做）
scope: 只回答"能不能做、代价是什么、风险在哪"；不含实现
---

# HDR 管线调研

> 触发问题（用户原话）："看下 vfx 的 sdr -> hdr 实现，另外我们的管线目前会把 hdr 内容转 sdr 再处理，
> 新版 vfx 应该可以直接支持 hdr 内容，mpv 也支持，先做调研吧，暂时不开发。"

## 0. 结论摘要

1. **VFX 的 "SDR→HDR" = `TrueHDR` effect，方向和我们想的相反**：它吃 **SDR RGBA U8**、
   吐 **HDR10（BT.2020 + PQ，打包 RGB10A2）**。它不能处理 HDR 输入，也不是"HDR 内容支持"。
   已有实测（`docs/vfx-sdk-1.3-investigation.md` §4）：默认参数下平均饱和度降 6–15%、
   `DebandingOff` 在样本上逐位无差异；**只在有 HDR 输出时才值得接**。
2. **"新版 VFX 直接支持 HDR 内容"这句成立，但指的是 1.3 新增的 10-bit 通路**：
   `VideoSuperRes`（`ImageEncodingMode=1` → RGB10A2/P32）与 `VideoFrameGeneration`（自动按输入
   格式，RGB10A2 可用）都能跑 10-bit。**我们目前是 8-bit**，这是明确的画质升级点。
3. **我们管线的真实路径（有代码证据）不是"先转 SDR 再处理"，而是"10-bit PQ 量化成 8-bit PQ
   再处理，最后在 mpv VO 里 tonemap 成 SDR"**：
   - `yuv_to_rgba` 的 CUDA kernel **完全不看 transfer 曲线**（只做 BT.2020/709/601 矩阵 +
     limited/full range 归一化），输出 `NVCV_U8`/RGBA8 → PQ 码值被原样压到 8-bit（每个码步 ≈ 3.7–4.0%
     亮度级，10-bit 是 0.9–1.0%，**粗 4 倍**）；
   - 滤镜链输出仍带 PQ/BT.2020 元数据（`vf_vsr.c:776`），所以画面"还是 HDR"，
     但 AI（VSR/VFG）看到的是 8-bit PQ 数据（非模型训练分布）；
   - 真正的 HDR→SDR 发生在最后：mpv 的 `vo_gpu` 渲染器在渲染到我们的 **RGBA8 FBO** 时
     tonemap 到 gamma2.2 / 203 nit / BT.709（`video.c:2839-2858` + `csputils.h:74`）。
4. **mpv 本身 HDR 能力齐全，但我们的集成把它锁死在 8-bit SDR**：自建 Vulkan render backend
   有 3 处硬编码 8-bit（`vk_color_depth()=8`、`pl_vulkan_wrap` 格式 RGBA8、`ra_fbo.color_space`
   返回 `{0}`），Qt 侧还有 2 处（rtImage RGBA8、composite render pass B8G8R8A8）。
   其中 `ra_fbo.color_space` 正是 "宿主会按什么色彩空间解释这个 FBO" 的官方钩子。
5. **HDR 输出（真 HDR 上屏）是独立的一大块，但本机可端到端验证**：Qt 6.12 只有**私有**的
   `QRhiSwapChain::Format::HDR10` / `QRhiSwapChainHdrInfo`，Qt Quick 没有公开 HDR API → 需要 spike；
   显示器侧没问题（用户 2026-10-17："显示器虽然亮度不行，但是 hdr 还是能开的，这个问题不用纠结"）：
   EDID 声明 ST2084 + BT2020，峰值 **400 nit**、基色 ≈ **DCI-P3**（见 §6）。
6. **建议顺序**：① 10-bit 内部通路（收益确定、不碰显示侧）→ ② HDR 输出 spike →
   ③ 最后才考虑 TrueHDR（并且要和 mpv 自带的 `--inverse-tone-mapping` 对比，后者零成本）。

## 1. 名词澄清（这是本轮最容易混的两件事）

| | TrueHDR | 10-bit 通路（RGB10A2） |
|---|---|---|
| 方向 | **SDR → HDR10** | **HDR 保持 HDR**（不换色彩空间，只保精度） |
| 输入 | RGBA U8（SDR，Rec.709/D65/gamma2.2 全域，**不识别源色彩空间**） | RGBA U8 **或** RGB10A2/P32 |
| 输出 | RGB10A2，**PQ 编码的 HDR10**，BT.2020 | 与输入同格式 |
| 尺寸 | **必须与输入同尺寸**（不能放大） | VSR 可放大；VFG 同尺寸插帧 |
| 收益场景 | SDR 老片在 **HDR 屏**上更好看 | HDR 片源在**任何**屏上不再被 8-bit 量化 |
| 依赖 | 有 HDR 输出才有意义 | 与显示侧无关 |

→ 用户说的"直接支持 HDR 内容" = 表格右列；"SDR→HDR" = 左列。两者互不替代。

## 2. VFX TrueHDR 实现（证据：SDK 库内自带的参数文档字符串）

`strings libnvVFXTrueHDR.so` 直接给出官方参数说明（这是库自己打印的 API 文档，比 docs 站更硬）：

```
SrcImage0     (NvCVImage*)   - input image RGBA (U8, Interleaved format), in NVCV_CUDA or NVCV_CPU_PINNED memory
DstImage0     (NvCVImage*)   - output image NVCV_RGB10A2 (P32 packed, Interleaved format), in NVCV_CUDA or NVCV_CPU_PINNED memory, HDR10 PQ encoded
BatchSize     (unsigned int) - batch size (only 1 supported)
Contrast      (unsigned int) - contrast tunable, default 100, range [0..200]
Saturation    (unsigned int) - saturation tunable, default 100, range [0..200]
MiddleGray    (unsigned int) - middle-grey reference, default 50, range [10..100]
Luminance     (unsigned int) - target HDR display peak luminance in nits, default 650, range [400..2000]
DebandingOff  (unsigned int) - set to 1 to skip the DL-Debander pass for higher throughput on clean sources, default 0, range [0 or 1]
```

### 2.1 内部结构

- 实现 = `NvVFXTrueHDRImpl`（导出符号 `NvVFXTrueHDRImpl`、`15VFXFilterBaseV0`），
  底层是 **NGX `libnvidia-ngx-truehdr.so.1.2.3`**（SDK 自带，和驱动级 "RTX Video HDR" 同源模型），
  并串一个 **DL-Debander** pass（`DebandingOff=1` 可跳过）。
- 数据通路：`AllocateInputBuffer` / `CopyInputToArray` 把输入拷进 **CUDA array**（`cuArrayCreate`），
  用 `cuSurfObjectCreate` / `cuTexObjectCreate` 做 surface/texture 读写，
  再 `CopyArrayToOutput` 写出；`BatchSize` 只支持 1。
- 内部还有 `TrueHDR.InLeft/InTop/InRight/InBottom`、`TrueHDR.OutLeft..OutRight`、
  `TrueHDR.MaxLuminance` 等 crop/参数槽（效果等价于参数 `Luminance`）。
- 强制校验（错误字符串逐条可读）：
  - 输入必须是 `NVCV_RGBA` + `U8` + chunky；BGRA 要应用层先转 RGBA；
  - 输出必须是 `NVCV_RGB10A2` + `NVCV_P32` + chunky，**且尺寸与输入一致**；
  - 非 CUDA / pinned 内存直接拒。

### 2.2 质量实测（引用 `docs/vfx-sdk-1.3-investigation.md` §4，4 张老 JPEG 静图）

| 指标 | 原图 | TrueHDR（lum400） |
|---|---|---|
| 中位亮度 | 20.6 / 27.8 / 42.6 / 31.9 nit | 48.3 / 65.8 / 105.5 / 76.1 nit（×2.35–2.47） |
| 高光 p99 | 93–99 nit | 341–389 nit（×3.7–3.9） |
| 平均饱和度 | 0.298–0.365 | 0.254–0.328（**默认参数下降 6–15%**） |
| 色域越界（BT.2020→709） | — | 1.4–7.7%（SDR 屏显示不出的部分） |
| `DebandingOff` A/B | — | **max\|Δ\|=0，非零像素 0.000%**（样本上无作用） |

结论：**它是"上变换"而不是"还原"**；在没有 HDR 屏的机器上接它 = 多一次推理 + 再 tonemap 回 SDR，
纯亏。只有 HDR 输出落地后才有讨论价值。

### 2.3 未测（诚实记录）

- **视频吞吐没测**：之前只有静图（4.8–7.4 ms/张 的 VSR 4× 数据，TrueHDR 没记时间）。
  本机现在没有 torch/cupy（`pynvvfx` 需要 DLPack 数组库），无法复跑；要测需写 C 探针或装依赖。
- 1080p/4K 实时性、与 VSR/VFG 串联后的总开销、显存峰值：全部未知。

## 3. 我们当前对 HDR 内容做了什么（逐段落证据）

测试素材：`input/Fallout S02E02 2025 2160p AMZN WEB-DL HDR ...mkv`
`ffprobe` = `hevc / yuv420p10le / bt2020nc / smpte2084 / tv range / 3840x1600`。

```
decode(nvdec) → CUDA P010 (10-bit PQ) → [vf_hwup 仅软解] → vf_vsr
   └─ yuv_to_rgba (NVRTC) : 10-bit YUV + BT.2020 矩阵 + limited range → RGBA **U8**（无 EOTF）
   └─ VSR 引擎 : NVCV_RGBA / NVCV_U8          ← 8-bit
→ vf_vfg : NVCV_RGBA / NVCV_U8                ← 8-bit
→ VO (vo_libmpv → vo_gpu) : 帧仍标 PQ/BT.2020 → tonemap 到 gamma2.2 / 203 nit / BT.709
→ 我们的 Vulkan rtImage (R8G8B8A8_UNORM) → composite → Qt swapchain (B8G8R8A8)
```

关键代码位置：

| 位置 | 事实 |
|---|---|
| `src/mpv/video/filter/yuv_to_rgba.c:47-82` | kernel 只有矩阵 + range 归一化 + `__saturatef` + `(uchar)(r*255+0.5)`；**没有 min(transfer/primaries)**，即 PQ 只被当数值处理 |
| `src/mpv/video/filter/vsr_proc.c:38` | `NvCVImage_Alloc(..., NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32)`（out/in/tmp 全 8-bit） |
| `src/mpv/video/filter/vfg_proc.c:31` | 同上（VFG 的 src0/src1/dst 全 8-bit） |
| `src/mpv/video/filter/vf_vsr.c:688-691` | 位深来自 `mp_imgfmt_get_desc(hw_subfmt).bpp[0]`（P010 → 16），矩阵来自 `matrix_from_repr`，**range 来自 levels**，三者是转换的全部输入 |
| `src/mpv/video/filter/vf_vsr.c:776` | `out->params.color = mpi->params.color; // preserve HDR metadata` → 8-bit RGBA 帧仍宣称自己是 PQ |
| `third_party/mpv/video/out/gpu/video.c:2839-2858` | FBO 未给色彩空间时：`dst.transfer = src.transfer`，HDR 源强制改成 `GAMMA22`；`dst.hdr.max_luma = nominal_peak(GAMMA22) * MP_REF_WHITE` = **203 nit** |
| `third_party/mpv/video/csputils.h:74` | `#define MP_REF_WHITE 203.0` |
| `src/mpv/video/out/vulkan/libmpv_vk.c:62` | `static int vk_color_depth(...) { return 8; }`（告诉 mpv 目标位深，决定 dither 到 8-bit） |
| `src/mpv/video/out/vulkan/libmpv_vk.c:98` | `pl_vulkan_wrap` 的 `.format = VK_FORMAT_R8G8B8A8_UNORM`（忽略调用方传入格式） |
| `src/mpv/video/out/vulkan/libmpv_vk.c:64` + `:125` | `vk_target_csp()` 返回 `{0}`；`*out_fbo = {.tex=..., .flip=false}` → `ra_fbo.color_space` 为 0 |
| `src/client/Video.cpp:195,267,359` | rtImage / imageView / `mpv_->render(...)` 全是 `VK_FORMAT_R8G8B8A8_UNORM` |
| `src/client/CompositePipeline.cpp:30` | render pass attachment = `VK_FORMAT_B8G8R8A8_UNORM`（钉死 Qt swapchain 格式） |
| `src/client/MpvController.cpp:43,50` | 只设了 `vo=libmpv`、`hwdec=nvdec`；**没有任何** `target-trc/target-prim/target-peak/tone-mapping` 设置 |

### 3.1 8-bit PQ 到底损失多少（数值）

PQ（ST 2084）是感知编码，8-bit 并不会"完全崩"，但码步明显变粗：

| 亮度级 | 8-bit 每码步 | 10-bit 每码步 |
|---|---|---|
| 0.5 nit | 0.047 nit（**9.5%**） | 0.012 nit（2.3%） |
| 2 nit | 0.135 nit（6.8%） | 0.033 nit（1.7%） |
| 10 nit | 0.521 nit（5.2%） | 0.125 nit（1.3%） |
| 100 nit | 4.04 nit（4.0%） | 0.98 nit（1.0%） |
| 1000 nit | 37.0 nit（3.7%） | 9.08 nit（0.9%） |

即：**我们把源头的 10-bit（或更高）PQ 先压成 8-bit，再交给 AI 放大 2–4×**，
量化台阶会被放大成可见色带/块，同时暗部细节（<5 nit）单步误差接近 10%。
这不是"色调错了"，而是"精度没了 + AI 输入不在训练分布内"。

### 3.2 补充：HDR 内容在 SDR 屏上现在的观感链路

现在的 tonemap 是 mpv 的默认（BT.2390 → 203 nit gamma2.2 + gamut 映射到 BT.709），
**能用**，但：① 目标是 203 nit 固定参考白，不随显示器实际亮度；② 输入已经 8-bit；
③ 高光裁剪/色域映射在 8-bit 空间做，色带更明显。

## 4. 新版 VFX 的 10-bit（HDR）通路能力

### 4.1 VideoSuperRes（1.3 新增）

`strings libnvVFXVideoSuperRes.so`：

```
SrcImage0     - input image, one of: (RGBA|BGRA)+U8 chunky 8-bit, or RGB10A2+P32 chunky 10-bit (packed R10G10B10A2). In NVCV_CUDA or NVCV_CPU_PINNED memory
DstImage0     - output image, same format/depth as input ...
ImageEncodingMode (unsigned int) - encoding/bit depth (default 0): 0=8-bit RGBA/BGRA, 1=10-bit RGB10A2. Set before Load(); input/output images must match this mode
%s image bit depth (%s) does not match configured ImageEncodingMode (%s); set NVVFX_IMAGE_ENCODING_MODE before the image
```

→ 10-bit 是**显式开关 + 显式格式**：`ImageEncodingMode=1` 必须在 `Load()` 前设，
输入输出都必须是 RGB10A2/P32；混用 8/10-bit 直接被拒。我们当前是默认 0。

### 4.2 VideoFrameGeneration（1.3 同时支持）

```
SrcImage0     - previous frame: (RGBA/BGRA)+U8 or RGB10A2+P32 ...
SrcImage1     - current frame: same encoding as previous frame ...
DstImage0     - interpolated output frame: same encoding/size as inputs ...; RGB8/RGB10A2 mixing is unsupported
```

→ VFG 没有 encoding-mode 开关，**按图像格式自动判定**，只要三张图（prev/cur/dst）一致即可。
（也就是说 10-bit 通路里 VFG 侧改动很小，主要在 `vfg_proc.c` 的分配格式。）

### 4.3 与 mpv/FFmpeg 的对接面（本项目内已有支持）

- mpv 有 `IMGFMT_X2RGB10`（别名 `rgb30`，`video/img_format.h:250`，`img_format.c:152`），
  FFmpeg 对应 `AV_PIX_FMT_X2RGB10`（`fmt-conversion.c:55`）→ 10-bit 打包 RGB 帧格式现成。
- CUDA→Vulkan 互操作有 `video/out/hwdec/hwdec_cuda_vk.c`（我们已在用这条链）。
- 带宽**不增加**：RGBA8 与 RGB10A2 都是 32 bpp，同样的 pitch/拷贝量（VSR/VFG 的 D2D 拷贝不变）。

## 5. mpv 侧 HDR 支持现状

| 能力 | 状态 |
|---|---|
| HDR→SDR tone mapping | ✅ `vo_gpu` 完整支持（默认 `tone-mapping=auto`→BT.2390，`hdr-compute-peak=auto`） |
| SDR→HDR（反 tone mapping） | ✅ 有 `--inverse-tone-mapping` + `--tone-mapping-max-boost`（算法级，非 AI） |
| HDR 输出（PQ/BT.2020 进 10-bit FBO） | ✅ 渲染器支持：把 `ra_fbo.color_space` 填成 `{PQ, BT.2020, max_luma}` + `color_depth=10` + 10-bit 格式即可；也可用 `--target-trc=pq --target-prim=bt2020 --target-peak=N` 覆盖 |
| 显示器色彩空间自动探测 | ❌ `--target-colorspace-hint` 只存在于 **vo_gpu_next**（`vo_gpu_next.c:223`），我们用的是 vo_gpu + 自建 Vulkan backend → **拿不到**；必须由宿主（Qt/KWin）提供 |
| 渲染 API 的 HDR 元数据参数 | ❌ `MPV_RENDER_PARAM_*` 没有 HDR/色彩空间项（`render.h` 只有 ICC/ambient light） |

要点：**mpv 侧不缺能力，缺的是我们这条自建 backend 的 3 个 8-bit 硬编码 + FBO 色彩空间没填。**
其中 `ra_fbo.color_space` 的语义在 mpv 源码里写得很直白（`video/out/gpu/context.h:105-112`：
"Host system's colorspace that it will be interpreting the frame buffer as"）。

## 6. Qt / KWin / 显示器侧

| 环节 | 现状（本机实测） |
|---|---|
| Qt 6.12 | `QRhiSwapChain::Format::HDR10` 存在，但只在**私有头** `QtGui/6.12.0/QtGui/rhi/qrhi.h:1615`；还有 `QRhiSwapChainHdrInfo`（min/max luminance、`sdrWhiteLevel`、LuminanceBehavior）——同样是私有 API |
| Qt Quick | 公开头里**没有** HDR/colorSpace 接口（`qquickwindow.h` 无相关符号）；只能用私有 API 或 Qt 未来的公开接口 → **必须 spike** |
| `QSurfaceFormat` | 有公开的 `setColorSpace(QColorSpace)`（`qsurfaceformat.h:116-117`），可能是 Wayland surface 色彩空间的入口（待验证） |
| 我们的 compositor | render pass 钉死 `B8G8R8A8_UNORM`（`CompositePipeline.cpp:30`），rtImage 钉死 RGBA8 → 两处都要改 |
| KWin | 6.8.0（`kwin_wayland --version`）；支持 `wp-color-management`（mpv `wayland_common.c` 里有完整映射） |
| 显示器 | DP-3 = **LG ULTRAFINE**（GSM，27" 4K）。`kscreen-doctor -o` 当前 `HDR: disabled`（**可开**，用户确认，不纠结）；`edid-decode` 给出实际能力：EOTF 含 **SMPTE ST2084**，`Desired content max luminance = 400 cd/m²`、maxFALL 400、min 0.39，CTA 声明 **BT2020RGB/BT2020YCC**，基色 R(0.681,0.316) G(0.258,0.676) B(0.150,0.062) ≈ **DCI-P3**，白点 (0.313,0.329) |

→ 本机 HDR 输出的**可验证范围**：400 nit 峰值 + P3 级色域 + 10-bit，对比现在的 203 nit / BT.709 / 8-bit，
差异足够明显（足以判"通/不通"）；但**不适合**验证高动态 Range 极限（1000+ nit 素材必须压到 400 nit，
`target-peak` 只能填 400，且 tonemap 不能关，否则高光会硬裁）。

## 7. 方案与代价

### 方案 A（✅ 2026-10-17 已实施）：10-bit 内部通路，输出仍是 SDR

链路改为：`P010 → YUV10→RGB10 CUDA kernel → VSR(RGB10A2) → VFG(RGB10A2) → VO（PQ 元数据保留）→ mpv tonemap → 8-bit SDR 渲染目标`。

- 收益：AI 输入不再是 8-bit PQ（码步粗 4 倍 → 回到 10-bit）；暗部色带/量化台阶消除；
  高光 tonemap 在 10-bit 空间做；**不碰 Qt/合成器/显示器**。
- 代价：① `yuv_to_rgba` 要加 10-bit 输出变体（打包 `R10G10B10A2`）；
  ② `vsr_proc.c`/`vfg_proc.c` 换格式 + `ImageEncodingMode=1`；
  ③ `vf_vsr`/`vf_vfg` 的 hwfmt 换 `IMGFMT_X2RGB10`；
  ④ **NVCV 的 `RGB10A2` / `P32` 常量数值仍未验证**（上一轮 `truehdr_fmt.c` 探针枚举到 pf=12 就静默退出；
  wheel 不带 `nvCVImage.h`）→ 实现前必须先用探针/反汇编钉死这两个值；
  ⑤ 引擎 10-bit 性能未知（同 bpp，理论上接近，但要实测）。
- 风险：中低。失败可回滚到 8-bit（单开关）。

**实施结果**（细节与坑见 `AGENTS.md`「10-bit（HDR）通路」）：链路已通，VSR/VFG 都是 RGB10A2，VO 直接吃 `cuda[x2bgr10]`（需 mpv overlay 补丁 `video/out/placebo/ra_pl.c`，否则退回 CPU hwdownload）。8-bit SDR 内容仍走 RGBA8，零回归。实测 10-bit 引擎耗时与 8-bit 相同（960x540→1080p 1.36 vs 1.43 ms；1080p→4K 5.39 vs 5.69 ms）；4K HDR 上 VSR 1:1 denoise 16.6 ms + VFG 2.9 ms，24fps 源跟得上。合成 BT.2020/PQ 色条（420）与 mpv 原生路径逐通道吻合（148.0/0/40.8 vs 145/0/39.7）。遗留：色度 range 归一化偏差 ~6%、软解 10-bit 经 hwup 偏暗、444/422 输入未支持（均见 AGENTS.md 的"已知遗留"）。

**A 的一个变体（值得单列）**：`P010 → tonemap 到 SDR 10-bit → VSR/VFG`。
AI 看到的是训练分布内的 SDR 数据，理论上画质更好；但需要在滤镜链里加一个 GPU tonemapper
（libplacebo/CUDA），而且之后 mpv 会把它当 SDR 处理——等于放弃 HDR 元数据，与"支持 HDR 内容"的目标背离。
建议做成 A/B 截图对比（`vf-command @vsr dump-both`）后再定。

### 方案 B：HDR 输出（真 HDR 上屏）

- mpv 侧：`ra_fbo.color_space` 填 PQ/BT.2020/峰值、`color_depth=10`、`pl_vulkan_wrap` 用
  `A2B10G10R10_UNORM_PACK32`、rtImage 同格式、composite render pass 同格式。
- Qt 侧：**必须激活 HDR10 swapchain + surface 色彩空间**——公开 API 缺失，
  需要 spike（私有 `QRhiSwapChain::setFormat(HDR10)` + `QRhiSwapChainHdrInfo`，
  或等 Qt 公开接口）。
- 系统侧：KWin 里打开 HDR（DP-3 支持，用户确认；当前设置里是 disabled）→ 开完 `kscreen-doctor -o`
  应显示 `HDR: enabled`，此时 Qt 侧应能拿到 `QRhiSwapChainHdrInfo` 的 400 nit / sdrWhiteLevel。
- 还要补：HDR10 静态元数据（MaxCLL/MaxFALL/母版显示）由谁发给合成器——TrueHDR 只输出 PQ 像素，
  不产出元数据；而 render API 没有元数据参数 → 只能走 Qt/KWin 的接口。
- 代价/风险：**高**（主要风险集中在 Qt 私有 API 能否激活 HDR10 swapchain，以及 HDR10 静态元数据
  由谁上报）；本机可端到端验证，但只有 400 nit 峰值（见 §6），验证的是"400 nit HDR vs 203 nit SDR"，
  不是高动态范围极限。

### 方案 C：TrueHDR（SDR 内容 → HDR 显示）

- 前置依赖：方案 B 必须先有（否则多一次推理再 tonemap 回 SDR，纯亏）。
- `Luminance` 取值：本机面板 400 nit → **只能取最小值 400**（默认 650 会超出面板能力，
  等于把 tonemap 压力推给下游）；这也意味着 TrueHDR 在本机的收益主要是"P3 色域 + 10-bit + 高光抬升"，
  而不是高动态范围。
- 位置：TrueHDR 是 1:1、输入 U8、输出 RGB10A2 → 只能放在 VSR **之后**（输出分辨率）或
  放在最前（源分辨率，之后 VSR/VFG 走 10-bit 通路）。前者贵、后者便宜且与方案 A 天然衔接。
- 输入归一化：TrueHDR 假定 Rec.709/D65/gamma2.2 **全范围**；我们 SDR 链可能是 limited/BT.601 →
  要先补一个规范化转换（这部分工作无论如何都要做）。
- 与 mpv 自带 `--inverse-tone-mapping` 的对比：后者零成本、算法级；TrueHDR 是 AI 级但贵。
  **先做客观对比（同静帧 PSNR/目检）再决定是否引入模型。**

## 8. 建议路线（不开发，仅建议）

1. **先做 A**：HDR 内容的 10-bit 精度修复，收益确定、风险可控、与显示侧解耦。
   第一步不是改代码，而是**用探针钉死 `RGB10A2` / `P32` 常量** + 量一次 10-bit VSR/VFG 吞吐。
2. **先做 B0（零代码，半天）**：验证"合成器 + 面板"这一段，把 Qt 问题隔离出来。KWin 开 HDR 后，
   用打包的 mpv CLI 直接跑同一片源（它走标准 `vo=gpu`，不经过我们的 Vulkan 后端）：
   ```
   ~/.local/bin/mpv-vsr --no-config --vo=gpu --gpu-context=wayland \
     --target-trc=pq --target-prim=bt2020 --target-peak=400 \
     "input/Fallout S02E02 2025 2160p AMZN WEB-DL HDR H 265 DDP5 1 Atmos-ADWeb.mkv"
   ```
   判据：① 画面比 SDR 路径明显更亮/更通透且不发灰；② `kscreen-doctor -o` 显示 `HDR: enabled`
   （KWin 接受并切了 PQ surface）；③ 截帧码值分布是"高光扩展"而不是"整体抬灰"。
   这步**不改任何代码**，却直接回答"我们的播放器接上 HDR 后是否有可见收益"。
3. **再做 B1 的 spike**：Qt 6.12 能否用私有 API 激活 HDR10 swapchain（花 0.5–1 天写最小 demo：
   一个 QQuickWindow + `QRhiSwapChain::setFormat(HDR10)` + 10-bit clear；用 `kscreen-doctor -o`
   确认 `HDR: enabled`，看 demo 是否触发 PQ 输出）。失败就明确"Qt 侧不具备条件"，把 B 挂起。
4. **C 最后**，且必须先有 B + 一次与 `inverse-tone-mapping` 的定量对比。

## 9. 证据清单（可复跑）

```bash
# VFX 能力（参数文档就在库里）
strings third_party/nvvfx/lib/libnvVFXTrueHDR.so | grep -E "SrcImage0|DstImage0|Contrast|Saturation|MiddleGray|Luminance|DebandingOff"
strings third_party/nvvfx/lib/libnvVFXVideoSuperRes.so | grep -E "ImageEncodingMode|RGB10A2|bit depth"
strings third_party/nvvfx/lib/libnvVFXVideoFrameGeneration.so | grep -E "SrcImage0|SrcImage1|DstImage0|mixing"

# 源素材色彩参数
ffprobe -v error -select_streams v:0 -show_entries \
  stream=pix_fmt,color_space,color_transfer,color_primaries,color_range \
  -of default "input/Fallout S02E02 2025 2160p AMZN WEB-DL HDR H 265 DDP5 1 Atmos-ADWeb.mkv"

# 显示/合成器能力（HDR 打开前后各跑一次）
kscreen-doctor -o | grep -E "HDR|Wide Color|Color profile|Allow EDR"
kwin_wayland --version
cp /sys/class/drm/card1-DP-3/edid /tmp/dp3.edid && edid-decode /tmp/dp3.edid | \
  grep -iE "product name|ST2084|max luminance|BT2020|Color Characteristics" -A1
grep -rn "HDR10" /usr/include/qt6/QtGui/6.12.0/QtGui/rhi/qrhi.h
```

参考：`docs/vfx-sdk-1.3-investigation.md`（TrueHDR 静图实测、VFG 接入要点、10-bit 待办）、
`docs/memory/vfx-no-trt10-needed.md`、`AGENTS.md`（滤镜链/渲染循环背景）。

## 10. HDR 上屏 spike 结果（2026-10-17 实测）

**结论先写**：Qt 6.12 + KWin 这条路**可用**，而且入口是"动态窗口属性 + 公开 API"，不是必须碰私有 API。
（注：本次 spike 只在开发机（KWin 6.8 + LG 400nit + 一台非 HDR Dell 作对照）验证机制，
**设计不得依赖本机面板参数**——峰值/色域/是否 HDR 全部按运行时从合成器读到的能力决定。）

### 10.1 关键坑：HDR swapchain 只在 **Vulkan** RHI 后端生效

- Wayland 下 Qt 默认走 **OpenGL(EGL)** 后端。此时：
  - `_qt_sg_hdr_format=hdr10` 被**静默忽略**（`QRhiSwapChain::format()` 恒 SDR）；
  - `isFormatSupported(HDR10/scRGB/P3)` **全 0**（GL 后端没有对应实现）；
  - 现象是 dmabuf fourcc 恒 `AR24`（8-bit）——我第一轮就踩了这个，并据此得出过
    "Qt 走不通"的错误结论（已作废）。
- 设 `QSG_RHI_BACKEND=vulkan`（本项目 app 本来就通过
  `QQuickGraphicsDevice::fromDeviceObjects` 走 Vulkan）后，同一份代码：
  - `isFormatSupported HDR10=1 scRGB=1 P3=1`；
  - `_qt_sg_hdr_format = "hdr10"` → **`QRhiSwapChain::format() == HDR10`**（真建出 HDR10 swapchain）。
- 也就是说：HDR 上屏的前提是**用 Vulkan 后端**（我们是），Qt 的 GL 后端不参与。

### 10.2 色彩空间声明：公开 API，KWin 认

`QSurfaceFormat::setColorSpace(QColorSpace(Primaries::Bt2020, TransferFunction::St2084))`
→ Qt 的 Wayland 客户端发 `wp_color_management` 请求（协议 XML 枚举值核对过）：

```
set_primaries_named(6)      # bt2020
set_tf_named(11)            # st2084_pq
set_image_description(...)
wp_image_description_v1.ready(...)   # KWin 接受
```

同一份 buffer 在三种声明下的观感确实不同（全屏窗口平均像素）：
sRGB `105.4` / PQ-BT2020 `102.8`（≈一致 → 往返正确）/ BT2020-Linear `138.1`（明显更亮）。
→ 证明声明**真的被合成器采纳**，且 PQ 下 Qt 会把 UI 按 PQ 编码，往返不偏色。

### 10.3 显示器无关性：必须解决的三件事

1. **真实显示能力不能靠 Qt**：Linux 上 `QRhiSwapChain::hdrInfo()` 只有 Windows 有实现
   （`#ifdef Q_OS_WIN`），其余平台返回 Qt 的硬编码默认值（min 0 / max 1000 nit /
   `sdrWhiteLevel` 200 nit）。Qt Quick 自己的 UI 会按这个默认值编码 → 在不同 HDR 显示器上
   观感会偏。真实峰值/最小亮度/色域要**从合成器读**（KWin 的
   `wp_color_management` output/surface feedback；mpv 的 wayland 后端已经这么读了：
   日志 `target: min_luma=… max_luma=… primaries=…`）。我们需要自建该协议客户端
   （项目已有 `WaylandIdleInhibit.cpp` 先例）或提供配置覆盖 + 合理兜底。
2. **格式选择要有 fallback 链且逐显示器判定**：`isFormatSupported()` 要求精确的
   `VkFormat + VkColorSpace` 配对（HDR10 = `A2B10G10R10 + HDR10_ST2084`，
   scRGB = `RGBA16F + EXTENDED_SRGB_LINEAR`）。不同合成器/驱动给的不同 →
   顺序应为 HDR10 → scRGB → SDR，且 `isFormatSupported` 是 **per-surface**：
   窗口移动到另一块屏后必须重新评估（多显示器混合场景是常态）。
3. **自建 Vulkan instance 要带 `VK_EXT_swapchain_colorspace`**（instance 扩展）：
   mpv/libplacebo 侧已启用；我们的 `VulkanContext.cpp` 目前没加，做 HDR 时要加。

### 10.4 运行期开关的边界（据 10.1–10.3 修正）

- 色彩空间声明：运行期可改（公开 API + Qt 的 pending image description 机制）。
- **swapchain 格式**（SDR 8-bit ↔ HDR10 10-bit）：从 SDR 屏切到 HDR 屏、或用户手动开关
  HDR 时都必须换格式 → 需重建 swapchain + Qt 的 swapchain render pass descriptor +
  我们自己的 composite render pass；Qt 的 `applySwapChainFormat()` 只在 swapchain 创建时调一次
  （`QSGRhiRenderLoop::ensureRhi`），运行期要自己驱动（导出私有符号可调），
  或者在换屏/切 HDR 时重建 QQuickWindow（约 100–300 ms 黑闪，mpv/VkDevice 不必重启）。
- 因此：**"支持任意 HDR 显示器"与"运行期无缝切换"是两个独立目标**；前者是必修，
  后者可以先做成"切 HDR = 重建渲染目标（必要时重建窗口）"。

### 10.5 spike 工装

`build/scratch/hdr_spike/`（gitignore）：`spike.cpp` + `spike.qml`，环境变量
`SPIKE_HDR`（请求 hdr10 swapchain）/`SPIKE_CS`（sRGB|pq|linear|p3）/`SPIKE_SCREEN`
（按名字选屏）/`SPIKE_VKEXT`/`SPIKE_FORCE`；输出 QRhi swapchain 实际格式、
`isFormatSupported` 三项、`surfaceForWindow` 句柄；配合 `WAYLAND_DEBUG=1` 看
`wp_color_management` 与 dmabuf fourcc。

## 11. 下一步实现清单（HDR 上屏；供后续会话直接照做）

**目标**：HDR 片源在真 HDR 显示器上以 **10-bit HDR10（PQ/BT.2020）** 输出；SDR 片源、
非 HDR 屏、不支持 color-management 的合成器**保持现状（8-bit SDR）**。能力全部运行时探测。
（单纯"10-bit 内部链路"不算完成——用户 2026-10-17："只是链路支持但不能 output 毫无意义"。）

### 11.1 改动点（按依赖顺序）

1. `src/client/VulkanContext.cpp`：instance 扩展加 **`VK_EXT_swapchain_colorspace`**
   （HDR 的 colorspace 枚举属于该 instance 扩展；mpv/libplacebo 侧已启用）。
2. **能力探测（建议第一步，零行为变更）**：窗口 show 后
   `QSGRendererInterface::RhiSwapchainResource` → `QRhiSwapChain*` →
   `isFormatSupported(HDR10 / HDRExtendedSrgbLinear / HDRExtendedDisplayP3Linear)`；
   监听 `QWindow::screenChanged`（`isFormatSupported` 是 **per-surface**，多屏混合必须重新评估）；
   日志（后续 OSD）输出实际选中格式。**必须走 Vulkan 后端**（见 §10.1，GL 后端全 false）。
3. **显示能力（峰值/最小亮度/色域）**：Qt 在 Linux 上 `hdrInfo()` 是硬编码默认值
   （min 0 / max 1000 nit / sdrWhiteLevel 200，仅 Windows 有真实实现）→ 自建
   `wp_color_management_v1` 客户端读 output/surface feedback（写法参考
   `src/client/WaylandIdleInhibit.cpp` 的自建协议 + mpv `wayland_common.c:20xx` 的映射）；
   读不到 → 退 SDR 或用户配置覆盖（不做硬编码假设）。
4. **窗口**：`show()` 之前 `view.setProperty("_qt_sg_hdr_format", "hdr10")`（fallback `"scrgb"`）+
   `QSurfaceFormat::setColorSpace(QColorSpace(Primaries::Bt2020, TransferFunction::St2084))`。
5. **渲染目标**：`Video::ensureRenderTarget` 的 rtImage 格式按 HDR 状态取
   `VK_FORMAT_A2B10G10R10_UNORM_PACK32`（HDR）/ `VK_FORMAT_R8G8B8A8_UNORM`（SDR）；
   `MpvController::render()` 传同一格式；`vkQueueWaitIdle` 后再销毁旧 image（已有习惯）。
6. **CompositePipeline**：render pass attachment **不能硬编码** `B8G8R8A8`——必须匹配 Qt 实际
   swapchain 格式（按 `sc->format()` 映射：SDR→`B8G8R8A8_UNORM`、HDR10→
   `A2B10G10R10_UNORM_PACK32`、scRGB→`R16G16B16A16_SFLOAT`），否则 pipeline 与
   render pass 不兼容（验证层报错/黑屏）。
7. **mpv 侧**（`src/mpv/video/out/vulkan/libmpv_vk.c`，我们的自建 backend）：
   - `vk_color_depth()` 现在恒返回 8 → HDR 时返回 10；
   - `pl_vulkan_wrap` 的 `.format` 现在硬编码 `R8G8B8A8_UNORM` → 用调用方传入的 VkFormat；
   - `vk_target_csp()`/`*out_fbo` 的 `ra_fbo.color_space` 现在 `{0}` → 填
     `{PQ, BT.2020, max_luma=<合成器峰值>}`（mpv `video/out/gpu/context.h:105` 的语义就是
     "宿主会按什么色彩空间解释这个 FBO"）；
   - `MpvController`：HDR 时设 `target-trc=pq` / `target-prim=bt.2020` / `target-peak=<峰值>`
     （这三个是 runtime option，`gl_video_update_options` 每帧重读）。
8. **切屏 / 手动切 HDR**：格式变化需重建 swapchain + Qt 的 swapchain render pass +
   我们的 composite render pass。Qt 的 `applySwapChainFormat()` 只在 swapchain 创建时调一次
   （`QSGRhiRenderLoop::ensureRhi`）→ 第一版接受"重建渲染目标 / 必要时重建 QQuickWindow"
   （窗口重建 ≈100–300 ms 黑闪，mpv 与 VkDevice 不必重启）；运行期无缝切换留第二步。

### 11.2 验收（每条都要有日志/截图/数值证据）

- `QRhiSwapChain::format()` 与 `isFormatSupported` 实测值（HDR 屏应为 HDR10=1、format=HDR10）；
- `WAYLAND_DEBUG=1` 看 dmabuf fourcc：HDR10 应为 10-bit（`AB30`/`XR30`），SDR 为 `AR24`；
- 色彩空间声明：`wp_color_management` 的 `set_primaries_named(6)` / `set_tf_named(11)` +
  `wp_image_description_v1.ready`（KWin 接受）；
- 画面：PQ 片源（如 `input/Fallout S02E02 …HDR…mkv`）与 `mpv --vo=gpu-next
  --target-colorspace-hint=yes` 的表现对比；SDR 屏/无协议环境仍能正常播放（退 SDR）；
- SDR 内容在 HDR 屏上不应比以前更差（Qt 按 `hdrInfo()` 默认值编码 UI，需目检确认）。

### 11.3 风险 / 未知

- Qt 在换屏后是否自动重建 swapchain（`isFormatSupported` per-surface）；
- Qt Quick 自己的 UI 在 HDR10 swapchain 上用的是硬编码 1000/200 nit 假设（Linux 无真实查询）
  → UI 亮度可能与桌面其他窗口不一致（Qt 既有限制，需目检/必要时上报）；
- wlroots / mutter 等合成器的 `wp_color_management` 支持未验证 → 必须"探测不到就退 SDR"；
- 运行期无缝切换需要能重建 Qt 的 `rpDescForSwapchain`（私有）→ 第一版不做。

### 11.4 spike 工装（可复跑）

`build/scratch/hdr_spike/`（gitignore）：`spike.cpp` + `spike.qml`；env：`SPIKE_HDR`（请求
hdr10）/`SPIKE_CS`（srgb|pq|linear|p3）/`SPIKE_SCREEN`（按名字选屏）/`SPIKE_VKEXT`（instance
加 `VK_EXT_swapchain_colorspace`）/`SPIKE_FORCE`（强设 QRhi 格式）；输出 `QRhiSwapChain::format()`、
`isFormatSupported` 三项、`surfaceForWindow`。**必须配 `QSG_RHI_BACKEND=vulkan`**。

## 12. 探测链路已落地（2026-10-17，分支 feat/10bit-hdr-path）

**已实现（零行为变更：只探测 + 打日志，不改任何上屏行为）**

1. `src/client/VulkanContext.cpp`：instance 扩展加 **`VK_EXT_swapchain_colorspace`**
   （先枚举可用性再决定加入——loader 不支持时只记日志，绝不让 `vkCreateInstance` 失败）。
   未启用时 `vkGetPhysicalDeviceSurfaceFormatsKHR` 只返回 SRGB 色彩空间 →
   Qt 的 `isFormatSupported(HDR10/scRGB/P3)` 恒 false。
2. `src/client/HdrOutput.{h,cpp}`：首帧（`afterRendering` 第一帧，此时 swapchain 才绑定
   窗口）探测 `QRhiSwapChain::isFormatSupported()/format()/hdrInfo()`；`screenChanged`
   重探；全部只打日志。`<rhi/qrhi.h>` 需要 qtbase 私有头目录（meson `_qt6_priv_inc`
   追加了 `<inc>/QtGui/<ver>/QtGui`，这是 Qt6::GuiPrivate 的标准布局）。
3. `src/client/WaylandColorManagement.{h,cpp}` → `libvsr-color-wayland.so`
   （独立 `wl_display` 连接 + 后台线程 dispatch，**不碰 Qt 的连接/事件线程**；
   与 `WaylandIdleInhibit.cpp` 同一"独立模块 + dlopen、失败即降级"模式）：
   bind `wp_color_management_v1` → 每个 `wl_output` 取 `get_image_description()`
   → `get_information()` → 解析 primaries/tf/luminances/target_*（与 mpv
   `wayland_common.c` 的 `info_*` 监听一一对应）。`HdrOutput` 按
   `QScreen::name()`（= `wl_output.name`）查询；描述变化回调 → `QMetaObject::
   invokeMethod(..., QueuedConnection)` 转投 GUI 线程重新评估。

**实测（KWin 6.8，DP-3 = LG 27" ULTRAFINE，DP-2 = DELL P2415Q）**

| 来源 | DP-3（HDR + WCG 开） | DP-2（HDR/WCG 均 incapable） |
|---|---|---|
| `isFormatSupported(HDR10/scRGB/P3)`（Qt） | 1 / 1 / 1 | **1 / 1 / 1（完全相同）** |
| swapchain 实际格式（未请求 HDR） | SDR | SDR |
| Qt `hdrInfo()` | min 0 / max 1000 nit / sdrWhite 200 | 同（硬编码，两屏一致） |
| 合成器 output description | **bt2020** / gamma2.2 / min 0 – max **400** nit / ref **124** | **srgb** / gamma2.2 / 0.01 – 200 nit / ref 63 |
| target(max_cll/fall) | 400 / 400 | 200 / 200 |
| `kscreen-doctor -o` | HDR enabled、WCG enabled、peak 400、SDR brightness 400 | HDR incapable、WCG incapable |

结论（都改写了 §10.3 的部分措辞）：

- **`isFormatSupported()` 在 Wayland 上是"合成器整体支持"，与窗口在哪块屏无关**：
  SDR 屏（DP-2）同样返回 HDR10=1。它只能回答"这个环境能不能建 HDR10 swapchain"，
  **不能**回答"这块屏现在是不是 HDR"（原文档"per-surface"的说法要按此修正：
  查的确实是该 surface 的 format 列表，但 KWin 对任意 surface 返回的都是全局列表）。
- 真实差异只在合成器的 **output image description**：primaries（bt2020 vs srgb）、
  亮度范围（400/124 vs 200/63）、target_*。
- **mpv 的 `max_luma > ref_luma ⇒ HDR` 判据在本机不成立**：DP-2 也是 200/63
  （headroom 3.17×），mpv 在 Dell 上同样打印 "Setting preferred transfer to PQ for
  HDR output"。根因：KWin 的 `reference_lum ≈ max_lum / π`（200/π≈63.7、
  400/π≈127≈124）——比例是构造出来的，不携带 HDR 信息。
- KWin 能力：`wp_color_management_v1 v2`，features{parametric、set_primaries、
  set_luminances、set_mastering、ext_volume、scrgb}（**无 icc、无 set_tf_power、
  无 windows_bt2100**）；tf 支持 bt1886/gamma22/ext_linear/**st2084_pq**/
  compound_power_2_4。→ 声明 PQ 可行（`vsr_color_wl_supports_tf(11)==1`）。
- ⚠ **未验证**：KWin 在 **WCG-only（HDR 关、WCG 开）** 状态下的 output description
  是 bt2020 还是 srgb。若是 bt2020，"WCG-only 屏"会被我们的判据当成可 HDR 输出
  （取舍见 §13）。
- 亮度口径：合成器亮度以 reference_lum 为参考，libplacebo 固定参考白 203 nit →
  按 mpv `wayland_common.c` `info_done` 的线性缩放换算（`HdrOutput.cpp` 的
  `PL_SDR_WHITE`）。DP-3 换算后 peak=654.8 nit（原始 400）、DP-2 644.5（原始 200）。

**验证工装**（均在 gitignore 的 `build/scratch/`）

- `hdr_cmspike/cmspike [--watch] [--verbose]`：独立 wayland 客户端直读
  `wp_color_management_v1`（不依赖 Qt），输出每个 output 的描述与派生判定。
- `hdr_probe/query_mod DP-3 DP-2`：dlopen 我们的模块做自检（与 app 同一条代码路径）。
- `hdr_probe/run*.log`：app 启动日志（`hdr probe[...]` / `hdr display[...]`）。
- 对照：`kscreen-doctor -o`（KWin 的真实 HDR 状态，含 sdrBrightness/peak）、
  `mpv --fs --fs-screen-name=DP-3 --target-colorspace-hint=yes --vo=gpu-next`（mpv 读到的
  同一份描述，日志 `[vo/gpu-next/wayland] Preferred surface feedback received`）。

## 13. 待定决策（**已于 §15/§16 关闭**，留档）

1. **HDR 判据（选屏）**：当前实现取"transfer 是 PQ/HLG **或** primaries 宽于
   sRGB/BT.709 ⇒ 可 HDR 输出"，读不到描述一律 SDR。待定：要不要把 WCG-only 排除
   （需先做 §12 的 WCG-only 对照实验）；给不给用户显式开关（`--hdr-output=auto|on|off`）。
2. **内容驱动 vs 显示驱动**：只在 HDR 片源时用 HDR10 swapchain（SDR 片源保持 8-bit
   现状，切换发生在换片边界），还是"显示器 HDR 就一直 10-bit"。前者与"不改变 SDR
   现状"一致，但 SDR↔HDR 切换需要重建 swapchain/渲染通道（§11.1 第 8 点）。
3. **Qt Quick 自己的 UI 在 HDR10 swapchain 上的观感**（Qt 用硬编码 1000/200 nit 假设，
   Linux 无真实查询）——需目检，必要时上报 Qt。

## 14. 强制 HDR10 切片（实现与验证，2026-10-17）

**范围**：只做"链路打通"的垂直切片——**强制模式**，不做自动决策、不做运行期切换。
默认行为必须与改动前逐字节一致。

### 14.1 开关与改动点

- 开关：env **`VSR_HDR=off|hdr10`**（默认/空/`off`/`0`/`sdr` → SDR = 现状；未知值记 ERR 后退 SDR）。
  `hdr10` 时在 `view.show()` **之前**：`setProperty("_qt_sg_hdr_format","hdr10")` +
  `QSurfaceFormat::setColorSpace(QColorSpace(Primaries::Bt2020, TransferFunction::St2084))`。
- `src/client/HdrOutput.{h,cpp}`：`requestedModeFromEnv()` / `qtHdrFormatProperty()` /
  `renderTargetFormatForMode()`（rtImage 用：SDR `R8G8B8A8_UNORM`、HDR10
  `A2B10G10R10_UNORM_PACK32`）/ `installSwapchainVerification()`（首帧后核对
  `QRhiSwapChain::format()`，不一致打 ERR，**不自动回退**）。
- `src/client/Video.{h,cpp}`：新增 `setRenderFormat()` + `renderFormat_/fmt_`；
  `ensureRenderTarget()` 的早退**同时比较格式**（格式变化即重建，销毁前仍
  `vkQueueWaitIdle`）；图像/视图/mpv 渲染都用同一格式。
- `src/client/CompositePipeline.{h,cpp}`：`create(..., VkFormat attachmentFormat)`，
  render pass attachment 不再硬编码 `B8G8R8A8`（必须匹配 Qt 实际 swapchain：
  SDR `B8G8R8A8_UNORM` / HDR10 `A2B10G10R10_UNORM_PACK32`）。
- `src/client/main.cpp`：模式决策 + 属性/色彩空间 + `setRenderFormat` +
  `comp.create(..., swapchainFmt)` + `mpv.setHdrTarget(...)`（亮度取
  `hdr::displayInfoFor()` = 合成器描述；读不到记 ERR 并退保守默认 max=1000/min=0）。
  另外把 loadFile 前的 SKIP_RENDERING pass 从硬编码 `R8G8B8A8` 改成
  `video->renderFormat()`（否则首帧 mpv 侧格式是错的）。
- mpv overlay（需 `scripts/build_mpv.sh` 重建）：
  - `src/mpv/include/mpv/render_vk.h`：`mpv_vulkan_image` 追加 `hdr` / `min_luma` /
    `max_luma`（0 = SDR，与旧调用方完全兼容）。
  - `src/mpv/video/out/vulkan/libmpv_vk.c`：`wrap_fbo()` 抄入宿主格式/HDR 目标 →
    `pl_vulkan_wrap_params.format = p->cur_format`（不再硬编码 RGBA8）；
    `vk_start_frame()` 的 `*out_fbo.color_space` 在 hdr 时填
    `{BT_2020, PQ, hdr={min_luma,max_luma}}`（**缓存快路径也带**，mpv 每帧调两次）、
    SDR 保持 `{0}`；`vk_color_depth()` 按 `cur_format` 返回 10/16/8；
    格式变化时打一条 `MP_INFO`（见 14.2 的 `libmpv_vk:` 行）。
- `src/client/MpvController.{h,cpp}`：`setHdrTarget(bool,min,max)` + `render()/skipRender()`
  填充 `mpv_vulkan_image` 的 HDR 字段（默认 SDR）。

### 14.2 实测（本机：DP-3=LG 400nit HDR 开 / DP-2=Dell 非 HDR；窗口由 KWin 决定落哪块屏）

SDR（默认，`VSR_HDR` 未设）——行为不变：

```
hdr: off（VSR_HDR 未启用）→ 保持 SDR 8-bit 现状
render target 2560x1440 fmt=37                     # R8G8B8A8_UNORM
[mpv info] libmpv_vk: render target fmt=37 hdr=0 color_depth=8 csp=unspecified
hdr verify: swapchain=SDR == 期望 SDR ✔（screen=DP-2）
wayland dmabuf create_immed 2560x1440 XR24(0x34325258)   # XRGB8888 = 8-bit
```

`VSR_HDR=hdr10`：

```
hdr: 强制 HDR10（VSR_HDR）—— _qt_sg_hdr_format=hdr10, colorSpace=BT.2020/ST2084,
     rtImage fmt=64, composite attachment fmt=64      # A2B10G10R10_UNORM_PACK32
mpv HDR target: HDR10 (PQ/BT.2020) (min=0.010 nit max=203.0 nit)
hdr: mpv target ← 合成器 screen=DP-2 peak=203.0 nit min=0.01 nit（primaries=1 tf=2）
render target 2560x1440 fmt=64
[mpv info] libmpv_vk: render target fmt=64 hdr=1 color_depth=10 csp=PQ/BT.2020
hdr probe[initial]: backend=Vulkan screen=DP-2 swapchain=HDR10 supported{HDR10=1 scRGB=1 P3=1}
hdr verify: swapchain=HDR10 == 期望 HDR10 ✔（screen=DP-2）
hdr probe[screen changed]: backend=Vulkan screen=DP-3 swapchain=HDR10 ...   # 换屏后仍是 HDR10
hdr display[screen changed]: screen=DP-3 primaries=6 tf=2 luminances{min=0.0 max=400.0 ref=400.0}
     → HDR-capable（primaries=6（宽于 sRGB/BT.709）） mpv口径 peak=203.0 nit min=0.00 nit
[mpv info] VO: [libmpv] 2880x540 cuda[x2bgr10]
[mpv status] vsr-status: mode=active scale=3.00 cost=0.9ms fps=25
wayland: wp_image_description_creator_params_v1.set_primaries_named(6)   # bt2020
         wp_image_description_creator_params_v1.set_tf_named(11)         # st2084_pq
         wp_color_management_surface_v1.set_image_description(...) + wp_image_description_v1.ready
wayland dmabuf create_immed 2560x1440 XB30(0x30334258)   # XBGR2101010 = 10-bit
```

- **0** 条 VUID/validation/"Failed to"/`[mpv error]`/`[mpv warn]`。
- 10-bit 与 PQ 声明都有直接证据（dmabuf fourcc XB30 vs XR24；协议 primaries=6/tf=11 + ready）。
- `peak=203.0 nit` 不是"屏只有 203 nit"：这是 mpv/libplacebo 的**参考白口径**
  （`PL_COLOR_SDR_WHITE=203`），与 mpv `wayland_common.c` 对同一描述算出的
  `pl_color_space.hdr.max_luma` 完全一致（DP-3：`(400-0)*203/400 = 203`），
  可直接喂给 libplacebo；填原始 400 反而会多出 ~2× headroom。
- 切片把窗口先放在 DP-2（Dell，非 HDR）再被 KWin 移到 DP-3；`isFormatSupported` 是
  "合成器整体支持"（两块屏都返回 1），**不能**判断某块屏是否真 HDR——这正是下一步
  "内容/显示驱动决策"要解决的问题（§13）。

### 14.3 本切片**未做**（后续）

1. 自动决策（HDR 片源 or HDR 屏 → 自动开）与运行期切换（切格式需重建 swapchain +
   Qt 的 swapchain render pass + 我们的 composite render pass）。
2. 换屏后重新读显示能力并更新 mpv target（本切片只在启动时读一次；DP-2→DP-3 的例子已证明必要）。
3. 色彩空间声明的动态更新（`QSurfaceFormat::setColorSpace` 运行期改，Qt 有 pending 机制）。
4. HDR10 静态元数据（MaxCLL/MaxFALL/母版显示）的上报。
5. `vk_color_depth()` 的顺序依赖：vo_gpu 只在 `resize()` 里读它（决定 dither 深度，
   不改格式），而 `resize()` 可能早于首个 `wrap_fbo` → 首次可能读到默认 8；窗口尺寸
   变化后会恢复正确（本切片接受，已在代码注释里写明）。

## 15. 选屏判据修正：只能用 **surface** preferred 描述（2026-10-17 实测）

§12 的判据（读 output image description 的 primaries）**被本轮实测推翻**：output
description 分不出 HDR 开关状态。改用 **合成器给这块表面的 preferred 描述**
（= mpv `--target-colorspace-hint` 用的同一个信号）后判据才成立。

**对照实验（`kscreen-doctor output.DP-3.hdr.enable|disable` 切换，cmspike --watch 观察）**

| 状态（同一块 LG，窗口在 DP-3） | output description | **surface preferred** | gamut_area | 判定 |
|---|---|---|---|---|
| HDR enabled | bt2020 / gamma2.2 / 0–400 / ref400 | **bt2020**(named=6) / gamma2.2 / 0–400 / ref124 | **0.2119** | HDR ✔ |
| HDR disabled（WCG 仍开） | bt2020 / gamma2.2 / 0–400 / ref400（**与上行逐字段相同**） | **bt709**(named=1) / gamma2.2 / 0.01–200 / ref63 | **0.1120** | SDR ✔ |
| DP-2（Dell，HDR/WCG 均 incapable） | srgb / gamma2.2 / 200 / ref200 | srgb / gamma2.2 / 200 / ref63 | 0.1120 | SDR ✔ |

- **判据 = "transfer 是 PQ/HLG" 或"基色三角面积明显大于 sRGB"**
  （面积：sRGB/BT.709=0.1120、DCI-P3≈0.1521、BT.2020=0.2119；阈值 1.15×sRGB）。
  用面积而不是逐基色枚举：面板无关、无需维护各标准值表。
- ⚠ `reference_lum` **不可用作判据**：同一状态下会在 `max/π`（63/124）与
  `sdrBrightness`（200/400）之间变（取决于客户端有没有声明色彩空间 / KWin 内部
  重算），而 `max>ref` 的 mpv 判据因此在本机不成立。
- 输出能力（400 nit、target_max）仍可从 surface preferred 拿到；`min/max/ref`
  换算到 libplacebo 参考白（203）的口径与 mpv `wayland_common.c` info_done 一致
  → DP-3 HDR 开且 ref=124 时 peak=654.8 nit（填给 mpv 是对的，别当物理 nit 显示）。

**实现：`src/client/WaylandColorFeedback.{h,cpp}` → `libvsr-color-feedback.so`**
（**取代** §12 的 `WaylandColorManagement`/`libvsr-color-wayland.so`，后者已删除）

- 为什么不能独立连接：surface feedback 必须在 **Qt 自己的 wayland 连接**上取
  （wl_surface 是连接私有的）→ 用 Qt 导出的协议封装
  `QtWayland::wp_color_manager_v1::get_surface_feedback()`（经
  `QWaylandDisplay::colorManager()` 拿管理器）。
- ⚠ **Qt 的便利类没有导出**：`ColorManagementFeedback` / `ImageDescriptionInfo` /
  `ColorManager` 都无 export 宏（`staticMetaObject` 都取不到）→ dlopen 时
  `undefined symbol`。只能基于**导出的协议封装**自己写 listener/解析（约 60 行）。
- ⚠ **初始 preferred 要主动取**：协议只在变化时发 `preferred_changed`；
  初次必须自己 `get_preferred()`（mpv 同样做法）。漏了会一直 `valid=0`。
- 回调在 Qt 的 wayland 事件线程 → 状态用 mutex；对应用的监听经
  `QMetaObject::invokeMethod(..., QueuedConnection)` 转投 GUI 线程。
- 实测：`libvsr-color-feedback.so` 在 DP-3（HDR）读到 `named_prim=6`,
  `gamut_area=0.2119`；WCG-only 与 DP-2 读到 `named_prim=1, gamut_area=0.1120`。

**运行期重新声明色彩空间（切片 ⑧ 的关键拼图，已验证）**

`QtWaylandClient::QWaylandWindow::initializeColorSpace()`（导出符号，dlopen 模块里
可调）会读 `window()->requestedFormat().colorSpace()` 并重新下发描述 →
`QWindow::setFormat(PQ)` + 调它 = **运行期把表面声明成 PQ/BT.2020**。
`WAYLAND_DEBUG=1` 实测（`build/scratch/hdr_probe/cf_test pq`，协议在 t=2.5s 出现）：

```
-> wp_color_manager_v1.create_parametric_creator(...)
-> ...set_primaries_named(6)      # bt2020
-> ...set_tf_named(11)            # st2084_pq
-> ...create(new id wp_image_description_v1)
<- wp_image_description_v1.ready(15880)
-> wp_color_management_surface_v1.set_image_description(<desc>, 0)
```

swapchain 侧的运行期切换（未实现，结论来自 Qt 源码）：`applySwapChainFormat()`
只在 swapchain 创建时读一次 `_qt_sg_hdr_format`（`qsgrenderloop.cpp` 的
`ensureRhi`）；运行期要自己
`sc->setFormat(QRhiSwapChain::HDR10)` → `sc->setRenderPassDescriptor(
sc->newCompatibleRenderPassDescriptor())` → `sc->createOrResize()`。
`qrhivulkan.cpp` 的 `createOrResize()` 会刷新 `rtWrapper` 的 render pass
（`rtWrapper.setRenderPassDescriptor(m_renderPassDesc)`）→ **Qt 每帧取
`currentFrameRenderTarget()` 即拿到新描述符**（`QQuickWindowPrivate::
rpDescForSwapchain` 只在 ensureRhi/清理处出现，无运行时读者）→ 理论可行，
但**待实测**（还需同时重建我们的 composite pipeline 与 rtImage、重设
`Video::setRenderFormat` + `mpv.setHdrTarget`）。

**状态（2026-10-17）**：① instance 扩展 ✔ ② swapchain 能力探测 ✔
③ 显示能力（surface preferred）✔ ④ 强制 HDR10 上屏 ✔（`VSR_HDR=hdr10`，§14）
⑤⑥⑦ 10-bit 渲染链路 ✔ ⑧ 运行期切换与自动决策 **未做**（设计已定，见上）。

## 16. 自动决策 + 运行期切换（2026-10-17，已实现并验证）

§11.1 的第 ⑧ 步与"自动决策"本轮落地。策略由 env `VSR_HDR` 控制：

| 值 | 行为 |
|---|---|
| `off` | 永远 SDR，且**完全不碰色彩空间声明**（= §14 之前的行为，逐字节不变） |
| `auto`（默认） | **内容 HDR × 显示能力 HDR** 才切 HDR10；否则 SDR |
| `hdr10` | 强制 HDR10（启动即切，验证/用户覆盖） |

判据（`hdr::installPolicy` 里的状态机，GUI 线程）：
- **内容 HDR** = mpv `video-params/gamma` ∈ {`pq`,`hlg`}（`PlayerViewModel::videoHdr`
  观察器，`videoHdrChanged` 信号触发重新决策）。
- **显示能力 HDR** = 合成器 surface preferred 描述（§15）→ `hdr_volume`
  （PQ/HLG 传输函数 或 基色三角面积 > 1.15×sRGB）。
- 读不到显示描述 / 内容非 HDR / 屏为 SDR 或 WCG-only → 一律 SDR（不假设）。

**运行期切换 `hdr::applyMode(window, mode)`**（两步都实测通过）：

1. `QWindow::setProperty("_qt_sg_hdr_format", ...)`（供 Qt 将来自己重建 swapchain 时沿用）
   + `QSurfaceFormat::setColorSpace(PQ/BT2020 | sRGB)` + **重新下发
   `wp_color_management` 声明**（`redeclareWindowColorSpace()` →
   `QtWaylandClient::QWaylandWindow::initializeColorSpace()`，§15 有协议级证据）。
2. `QRhiSwapChain::setFormat()` + `setRenderPassDescriptor(
   newCompatibleRenderPassDescriptor())` + `createOrResize()`（Qt 的 per-frame
   render target 在 createOrResize 里刷新，源码依据见 §15）。
3. 应用侧钩子 `rebuildRenderPath`：`CompositePipeline::create(..., 新 attachment 格式)`
   （可重入：`vkDeviceWaitIdle` 后销毁旧 render pass/pipeline）+ `Video::setRenderFormat`
   （置脏 → 下一帧**立即**重建 rtImage；否则 mpv 会用新格式写旧 image）
   + `video->setCompositeObjects()`；`updateMpvTarget`：
   `mpv.setHdrTarget(mode==Hdr10, minNits, peakNits)`（亮度取自 §15 的合成器快照）。

**实测（`VSR_HDR=auto`，HDR 片源 `build/scratch/hdr_bars10s.mp4`）**

```
795ms  hdr applyMode: swapchain SDR → HDR10（已重建，含 render pass）
795ms  hdr policy[content]: → HDR10（内容 HDR × 显示 HDR（gamut_area=0.2119，peak=203.0 nit））
804ms  hdr: 渲染链路 → HDR10（composite attachment=64 rtImage=64）
804ms  mpv HDR target: HDR10 (PQ/BT.2020) (min=0.000 nit max=203.0 nit)
805ms  render target 2560x1440 fmt=64
       libmpv_vk: render target fmt=64 hdr=1 color_depth=10 csp=PQ/BT.2020
       VO: [libmpv] cuda[x2bgr10] / vsr-status: mode=active（帧继续、25fps）
```
反向（RPC `loadfile` 切到 SDR 片源）：`hdr applyMode: swapchain HDR10 → SDR` →
渲染链路/ mpv target / rtImage 全部回 SDR（`fmt=37 hdr=0 color_depth=8`），无错误。
不满足条件时（内容 HDR 但 LG 的 HDR 关掉 = WCG-only，`gamut_area=0.1120`）**不切换**、
继续 SDR —— 即"非 HDR 屏保持现状"。

**⚠ 本轮踩到并修掉的真 bug（校验层抓到的）**：`CompositeRenderNode` 只在创建时
缓存 `pipe`/`pipeLayout`，而运行期切换会销毁并重建 composite pipeline → 在"没有
`updatePaintNode` 的帧"里节点会 `vkCmdBindPipeline` 一个**已销毁句柄**
（`VUID-vkCmdBindPipeline-pipeline-parameter: pipeline Invalid VkPipeline Object`）。
修法：节点持 `Video *owner`，**渲染阶段**从 owner 读当前 pipeline/layout
（`compositePipeline()`/`compositePipelineLayout()`），不再缓存。
校验层对照：`VSR_HDR=off` 与 `VSR_HDR=auto`（含运行期切换）的 VUID 集合与计数
**完全一致**（各 9 类）——切换不引入新校验错误。

**仍未做**：HDR10 静态元数据（MaxCLL/MaxFALL）上报；Qt Quick 自己的 UI 在
HDR10 swapchain 上的观感需**目检**（Qt 在 Linux 用硬编码 1000/200 nit 假设）；
`vk_color_depth()` 的 dither 深度在"只切换不 resize"时可能滞后（仅影响 dither）；
运行期切换的第一帧会有一次 swapchain 重建（无黑帧，但需目检确认无闪烁）。

## 17. "颜色偏红"根因与修复（2026-10-17，用户实测报告）

**现象**：同一 HDR 片源（Fallout 2160p HDR10，4K 24fps），我们的 app 画面**偏红/偏暗**，
mpv 原生（`--vo=gpu-next --target-colorspace-hint=yes`）正常。

**根因（两个叠加，均已修）**

1. **FBO 色彩空间被丢掉 → gl_video 把 PQ 内容烘成 gamma2.2 写进 PQ swapchain。**
   generic libmpv 渲染路径 `video/out/gpu/libmpv_gpu.c` 的 `render()` 自己拼
   `struct ra_fbo {.tex, .flip}`，**丢掉** backend（`libmpv_vk.c` 的
   `vk_start_frame()`）填的 `ra_fbo.color_space` → gl_video 视 FBO 色彩空间为
   "未指定" → `pass_colormanage()` 对 HDR 内容回退 `dst.transfer = gamma22`
   （源码：`if (dst.transfer == UNKNOWN) { dst.transfer = src.transfer;
   if (linear||hdr) dst.transfer = GAMMA22; }`）→ gamma2.2 编码的字节被合成器
   按 PQ 解释 → 暗 + 高光过饱和（观感"偏红"）。
   **修法**：宿主显式声明目标色彩空间 —— `MpvController::setHdrTarget()` 在 HDR 时
   `set target-trc=pq` + `target-prim=bt.2020`（gl_video 的这两个选项**优先于**
   fbo csp）→ 实测颜色恢复正常（R/G 0.858→0.904，高光由 (214,156,61) 回到
   (181,163,127)，mpv 参照 (199,174,129)）。
2. **预 tone map 到 203nit**（同时修掉）：一度把合成器亮度按 libplacebo 参考白
   缩放后填进 `ra_fbo.color_space.hdr.max_luma=203` → gl_video 本地把 HDR 压到
   SDR 白 → 暗 2.4 倍。**不要**把该值当峰值；`target-peak` 保持 auto
   （= 不预 tone map，交给合成器；与 mpv hint 模式一致）。

**剩余差异：我们自己下发 `wp_color_management` PQ 描述会让 KWin 明显变暗**

同尺寸（视频区宽 2564px）、播放中截图、中心区挖掉后的实测：

| 配置 | meanLum | R/G | B/G |
|---|---|---|---|
| mpv `--autofit=2560x1440`（hint=yes，**从不 set_image_description**） | **20.74** | 0.922 | 0.906 |
| ours 默认（**不自己声明**，靠 swapchain `VkColorSpace=HDR10_ST2084`） | **19.64** | 0.929 | 0.918 |
| ours `VSR_HDR_DECLARE=1`（自己声明 PQ/BT.2020 + luminances 0/10000/203） | 13.13 | 0.891 | 0.896 |

- 关键对照是**同一 app、同一窗口/尺寸/内容，只切"是否声明"**：19.64 vs 13.13
  → 声明 PQ 使 KWin 走另一条映射路径（忠实映射 PQ → 更暗）；mpv 在 Wayland 下
  **从不自己声明**（实测：只 `get_surface_feedback/get_preferred/get_information`，
  没有 `get_surface`/`set_image_description`），走的是"未声明 + swapchain 色彩空间"
  这条路径。
- 因此默认改为**不自己声明**（`VSR_HDR_DECLARE=1` 可切回声明路径对比）。
  声明路径语义更"自洽"（buffer 与声明一致），但在这台 KWin 上明显更暗；
  另外 `luminances(0,10000,203)` 的有无对结果**没有影响**（14.01 vs 14.01 实测）。
- ⚠ 若采用"不声明"，Qt 自己的 UI 仍按窗口 format 的 PQ 编码（观感与视频一致，
  但整块 surface 的最终解释依赖合成器默认）——**需目检**。

**方法论教训（本轮踩过，写下来避免重犯）**
- 亮度/颜色对比必须**同窗口尺寸**（我们 1280x720 *逻辑*@dpr2 = 2560x1440 设备像素；
  mpv 的 `--autofit` 是设备像素量级，用 `--autofit=2560x1440` 才能对齐）+
  **播放中截图**（暂停会显示播放按钮 overlay，`spectacle -a` 抓到的中心区被污染）
  + 统计时**挖掉中心区**。
- `spectacle -a` 会抓到"当前活动窗口"：mpv 因参数错误退出/启动失败时会抓到别的窗口
  （本轮两次抓到白窗口），必须自检抓到的图像内容。
- 用户报告优先当"测量"，不要急着用自己口径不干净的截图反驳或确认。

**附：本轮工具/复现**
- 抓帧：`build/scratch/hdr_color/`（`same_mpv_2560.png` / `same_ours_nodecl.png` /
  `same_ours_decl.png` + 同名 `.log`）；脚本 `/tmp/shot.sh`（launch → wait → `spectacle -a`
  → 自检图像非纯色 → kill）。
- 诊断日志：`vf_vsr.c`/`vf_vfg.c` 的 `yuv->rgb: bd=.. matrix=.. range=.. src sys=.. primaries=..`
  （本轮证明矩阵取值**是对的**：BT.2020_NC/limited/primaries=bt2020）。

### 17.1 修正：默认**声明 PQ**（用户目检 2026-10-17）

上表的"亮度更高"结论**是错的**，已按目检改正：

| 配置 | 用户目检 |
|---|---|
| 不下发自定义描述（对齐 mpv hint） | **画面发灰**（PQ 缓冲被按 gamma2.2 解释：更亮但饱和/对比塌陷） |
| 自己声明 PQ/BT.2020 + luminances（mpv 语义） | **画面正常** ✔ |

→ **默认改为声明**（`VSR_HDR_DECLARE=0` 才是"不声明"的对照路径）。
教训写在这里避免重犯：**平均亮度不是正确性判据**——"发灰"恰恰表现为均值更高；
颜色/亮度对比必须配合**目检**或饱和度/对比度指标，不能只看 meanLum。
（mpv 自己窗口看着正常是因为它走的是另一条 surface 路径（p010 + 未声明），
不能据此推断"我们的窗口不声明也对"。）

## 18. 交接（2026-10-17 会话末；供压缩上下文后直接续做）

### 18.1 分支与工作树状态

- 分支 `feat/10bit-hdr-path`（已推送的前 4 个 commit 是 10-bit 通路 + 文档：`6b92b02`/`5a81395`/`aeef6eb`/`63f30b4`）。
- **本轮全部改动尚未提交**（用户未说"提交"）。改动清单：
  - 新增：`src/client/HdrOutput.{h,cpp}`（能力探测 + 策略状态机 + 运行期切换）、
    `src/client/WaylandColorFeedback.{h,cpp}`（→ `libvsr-color-feedback.so`：读 surface
    preferred 描述 + 按 mpv 语义声明 HDR10；协议代码自生成）。
  - 修改：`VulkanContext.{h,cpp}`（instance 加 `VK_EXT_swapchain_colorspace`）、
    `main.cpp`（策略接线 + 钩子）、`Video.{h,cpp}`（`setRenderFormat` 置脏立即重建 +
    节点渲染期取 pipeline）、`CompositePipeline.{h,cpp}`（`create(..., VkFormat)` 可重入）、
    `MpvController.{h,cpp}`（`setHdrTarget` → `target-trc/prim`）、`meson.build`、
    `PlayerViewModel.{h,cpp}`（`videoHdr` = `video-params/gamma∈{pq,hlg}`）、
    `src/mpv/include/mpv/render_vk.h`（`mpv_vulkan_image` 加 hdr/min/max）、
    `src/mpv/video/out/vulkan/libmpv_vk.c`（wrap 用宿主格式/`color_depth` 按格式/
    亮度仅诊断）、`src/mpv/video/filter/vf_vsr.c`+`vf_vfg.c`（**临时诊断日志**
    `yuv->rgb: bd=.. matrix=.. range=.. src sys=.. primaries=..`，定位完可删）。
  - 文档：本文件 §12–§18、`AGENTS.md` HDR 小节。

### 18.2 运行开关

- `VSR_HDR=off|auto|hdr10`（**默认 auto**：内容 HDR × 显示能力 HDR 才切 HDR10；
  `off` 完全不碰色彩空间声明 = 逐字节现状）。
- `VSR_HDR_DECLARE`：**默认 1 = 我们自己下发 PQ/BT.2020 + luminances(0,10000,203)**
  （用户目检"画面正常"）；`=0` = 不下发（对齐 mpv hint 路径，本机**画面发灰**，仅作对照）。

### 18.3 本轮已修的真 bug

1. **`CompositeRenderNode` 缓存 pipeline 句柄** → 运行期切换后在没有 `updatePaintNode`
   的帧里绑已销毁 pipeline（校验层 `VUID-vkCmdBindPipeline-pipeline-parameter`）。
   修法：节点持 `Video*`，渲染阶段读 `compositePipeline()/compositePipelineLayout()`。
2. **HDR 目标色彩空间没送到 gl_video**（用户报告的"偏红"根因）：
   `video/out/gpu/libmpv_gpu.c` 的 `render()` 自己拼 `ra_fbo {.tex,.flip}`，**丢掉**
   `libmpv_vk.c` 的 `vk_start_frame()` 填的 `color_space` → gl_video 对 HDR 内容回退
   `dst.transfer=GAMMA22` → PQ 内容被烘成 gamma2.2 写进 PQ swapchain（暗 + 高光过饱和）。
   修法：宿主用 mpv 选项声明目标（`MpvController::setHdrTarget()` → `target-trc=pq`
   + `target-prim=bt.2020`）。**上游 `libmpv_gpu.c` 未改**（说明写在 `libmpv_vk.c` 注释里）。
3. 副产物：曾把合成器亮度按参考白缩放填进 `ra_fbo.color_space.hdr.max_luma=203`
   → 本地预 tone map 到 SDR 白（暗 2.4 倍）；已去掉（该值当前**仅诊断**）。

### 18.4 用户目检结论（权威）

- 声明 PQ：**画面正常** ✔（当前默认）；不声明：**画面发灰** ✗。
- 与 `mpv --vo=gpu-next --target-colorspace-hint=yes` 对比，**mpv 的对比度仍略好**。

### 18.5 下一步（按优先级，先分析已给出）

1. ~~**修 `src/mpv/video/filter/yuv_to_rgba.c` 的 limited-range 归一化**~~ ——
   **已修，见 §19**（2026-10-17 续做；正确跨度 8-bit 219/224、10-bit packed
   876/896、P010 56064/57344）。
2. 量化**渲染器差异**：同尺寸跑 `mpv --vo=gpu`（= 我们同族的 gl_video）vs
   `--vo=gpu-next`，再跑我们 `--fruc off --scale off`（这时 YUV→RGB 交给 libplacebo）
   —— 把"内核"与"渲染器"两个变量分开。（`--tone-mapping-mode` 等默认档差异**尚未逐行核对**。）
3. 我们的 kernel **色度无插值**（一对像素复用同一 Cb/Cr），libplacebo 会插值 → 彩色边缘
   更糊；属同类"细节/对比"差异。
4. 未做：HDR10 静态元数据（MaxCLL/MaxFALL，可照 mpv 的 `set_mastering_luminance`）；
   HDR10 下 **Qt 自己的 UI** 亮度需目检（Qt Linux 用硬编码 1000/200 nit 假设）。
5. 等用户说"提交"再 commit/push（含推送 `feat/10bit-hdr-path`）。

### 18.6 方法论/坑（本轮踩过，务必沿用）

- 亮度/颜色对比必须：**同窗口尺寸**（我们 1280×720 逻辑@dpr2 = 2560×1440 设备像素；
  mpv 的 `--autofit` 是设备像素量级）+ **播放中截图**（暂停会显示播放按钮 overlay）
  + 统计**挖掉中心区**；`spectacle -a` 抓"活动窗口"，抓错窗口会得到白图 → 必须自检图像。
- **平均亮度不是正确性判据**：发灰（PQ 被按 gamma2.2 解释）恰恰表现为**均值更高**。
  必须目检或看饱和度/对比度指标。
- **Qt 未导出的符号**：`QtWaylandClient::ColorManagementFeedback`/`ImageDescriptionInfo`/
  `ColorManager` 无 export 宏；Qt 导出的 `QtWayland::` 封装依赖其内部 `*_interface`
  符号，也没导出（dlopen 报 `undefined symbol: wp_image_description_v1_interface`）
  → 协议代码必须自生成（`wayland-scanner`，照 `WaylandIdleInhibit.cpp` 的模式绑定全局）。
- 初始 preferred 描述**必须主动 `get_preferred()`**（协议只在变化时发事件）；
  全局绑定可能在首次 `init()` 之后才到 → 模块在 `ColorManagerManagerExt::init()` 里补订阅，
  且 `ensureModule()` 对 `-2`（未就绪）要**每次重试**，不能只试一次。
- **写文件别用 `open(p,'w').write(open(p).read())`**：写模式先截断、再读到空 → 文件清零
  （本轮把 `HdrOutput.cpp` 清空过一次，已按 header/main.cpp 的 API 面完整重写）。
- `spectacle`/`mpv --target-prim` 等：mpv 的取值是 `bt.2020`（`bt2020` 会 fatal 退出）。

### 18.7 复现/证据路径（gitignore）

- `build/scratch/hdr_color/`：本轮同尺寸对照截图 + 日志（`same_mpv_2560.png`、
  `same_ours_nodecl.png`、`same_ours_decl.png`、`play_*.png`）。
- `build/scratch/hdr_slice/`（强制切片日志）、`hdr_slice2/`（auto/切换/换屏/开关 HDR 日志）、
  `hdr_probe/`（`query_mod` 模块自检、`cf_test` 声明路径验证、`run*.log`）、
  `hdr_cmspike/`（独立客户端直读协议、`--watch` 看 HDR 开关变化）。
- 临时抓帧脚本 `/tmp/shot.sh`（launch → wait → `spectacle -a` → 图像自检 → kill）。
- 诊断日志出处：`vf_vsr.c:595`、`vf_vfg.c:409`（`yuv->rgb:` 行）；`libmpv_vk.c:108`
  （`render target fmt=.. hdr=.. color_depth=.. csp=..`）。

---

## 19. limited-range 归一化修正（2026-10-17 续做；§18.5 第 1 项落地）

### 19.1 缺陷

`src/mpv/video/filter/yuv_to_rgba.c` 的 limited 分支用 `y_max - y_min` 当跨度
（8-bit `255-16=239`、P010 `65535-4096=61439`、10-bit packed `1023-64=959`），
**luma 与色度都错**（旧 AGENTS 记录只写了色度）。正确跨度 = 标准 limited 码域：

| 存储 | y_min | **y_span** | uv_mid | **uv_span** |
|---|---|---|---|---|
| 8-bit（NV12） | 16 | **219**（16..235） | 128 | **224**（16..240） |
| 10-bit packed | 64 | **876**（64..940） | 512 | **896**（64..960） |
| P010/P016（16bit 存储，MSB 对齐） | 4096 | **56064**（=876×64） | 32768 | **57344**（=896×64） |

**参考口径**：libplacebo `src/colorspace.c` limited 分支（7.360.1 已核对源码）——
`scale = 2^bits/(2^bits-1)`，`ymax=235/256*scale`、`ymin=16/256*scale`、
`cmax=240/256*scale`、`cmid=128/256*scale`，`ymul=1/(ymax-ymin)`、
`cmul=0.5/(cmax-cmid)`。展开后正是上表（8-bit → `(Y-16)/219`、`(C-128)/224`）。
ffmpeg swscale 同口径（`(Y-16)*255/219`，实测 16→0 / 235→255 / 180→191 / 213→229）。
**full-range 分支不动**（我们除满幅，与 libplacebo 的 `cmid=128/256*scale`
差 ~0.2%，历史全幅对账已吻合）。

症状幅度：白位只到满幅的 **91.6%**（8-bit 白 234/255、P010 白 934/1023）；
PQ 10-bit 条红 941 vs 参考 1010（≈8% 码值 → 高光亮度损失远大于 8%）。
这**同时**解释 §18.4 用户"mpv 对比度仍略好"（我们整体平/暗）。

### 19.2 数值对账（全部可复跑）

工具：`build/scratch/range/`（`probe.c` 直接驱动真实内核 · `compare.py` 对账
libplacebo 公式/ffmpeg · `e2e.sh`/`e2e_hdr.sh` + `e2e_check.py` 走真解码路径）。

1. **内核 vs 参考（合成 limited 图案，16 矢量：8 标准 75% 彩条 + 灰阶 + 越界值）**

   | 用例 | 旧内核 vs libplacebo | 新内核 vs libplacebo | ffmpeg vs libplacebo |
   |---|---|---|---|
   | 8-bit NV12，out 8-bit | mean\|Δ\| … max **21** | **max 0** | max 3（swscale 自身取整） |
   | P010，out 10-bit | max **89**（934 vs 1023） | **max 1** | — |

2. **端到端（真解码 → 内核 → `dump-both` 的 VSR 输入 PNG）**，素材
   `sdr_bars8.mp4`（yuv420p / tv / bt709，静态彩条，loop 成 30s）：
   - app dump **vs ffmpeg swscale 参考：mean\|Δ\|=0.444，max=2**
   - app dump vs 新内核公式：mean\|Δ\|=0.093（max<0.5 LSB）
   - app dump vs 旧内核公式：mean\|Δ\|=5.90，max=19 ← 修正前后的差距
   - app 日志确认 `bd=8 out=8 matrix=1 range=0 | src sys=2 levels=1`（BT.709/limited）

3. **10-bit PQ 路径（BT.2020 / tv / smpte2084 彩条 `hdr_bars420.mp4`）**，
   解码帧在 10-bit 码域与 ffmpeg 参考对账（BT.2020 系数、neighbor 色度）：
   - 旧内核 vs 参考：mean\|Δ\|=**23.76**，max=79（红条 941 vs 1010）
   - 新内核 vs 参考：mean\|Δ\|=**0.98**，max=3
   - **真机 E2E（`e2e_hdr.sh`，P010 解码帧 → dump 出的 8-bit PNG）**：
     app dump vs ffmpeg = mean 0.447 / max 2；vs 新内核公式 = 0.057；
     vs 旧内核公式 = 6.19 / max 21（红条 235 → 253）。日志
     `yuv->rgb: bd=16 out=10 matrix=2 range=0 | src sys=4 levels=1 primaries=6 transfer=12`
     确认走的是 P010 分支（bd=16 = 存储位深）。
   ⚠ AGENTS 里"合成三色条 ours vs mpv 逐通道吻合（148.0/0/40.8 vs 145/0/39.7）"
   是**修前**的 8-bit 屏幕截图口径（截图把码域差异压缩了），不能用来说明内核正确性。

### 19.3 覆盖范围与副作用

- 生效路径：`vf_vsr` / `vf_vfg` / `vf_rife` 共用同一内核（`ensure_yuv_converter`
  传 `range_from_levels(src->repr.levels)`），所以 **HDR 10-bit 与 SDR 8-bit
  limited 片源一起改变**。
- **不是**"改完又在下游被放大一次"：滤镜产出的帧是 `IMGFMT_CUDA` + `hw_subfmt
  =RGBA/X2BGR10`，`mp_image_params_guess_csp` 对 RGB 强制
  `repr.sys=RGB / levels=FULL`（`mp_image.c:523` 的 forced_csp 取 **hw_subfmt**；
  `mp_image.c:1019-1021`），`out->params.color` 只覆盖 primaries/transfer（保 HDR
  元数据）。即内核是**唯一**做范围展开的一层。
- ⚠ 观感影响是预期内的：SDR limited 片源白位 234→255、整体对比度上升；HDR 片源
  高光显著抬升（对齐 mpv/libplacebo 的正常表现）。**需要用户目检确认**（尤其
  之前"发灰/对比度略差"的判断要重新做一次）。

### 19.4 复跑

```bash
# 1) 内核离线对账（合成彩条）
cd build/scratch/range
gcc -O1 -o probe probe.c ../../../src/mpv/video/filter/yuv_to_rgba.c \
    -I../../../src/mpv/video/filter -I../../../third_party/mpv \
    -I../../../build/mpv/build -I/opt/cuda/include -L/opt/cuda/lib64 \
    -lnvrtc -lcuda -lm
./probe 8 8 r8 > after_8bit.txt            # 8-bit NV12（另跑 10 10 对 P010）
python3 compare.py after_8bit.txt after_p010_10bit.txt
# 2) 端到端（需要 GUI/合成器；约 10s，跑完自动 kill）
bash e2e.sh && python3 e2e_check.py          # SDR 8-bit 路径
bash e2e_hdr.sh                              # 10-bit PQ / P010 路径（dump 是 8-bit PNG）
```

- 证据文件：`build/scratch/range/{before,after}_8bit.txt`、`before/after_p010_10bit.txt`、
  `e2e_in.png`（SDR 内核输出）、`hdr_in.png`（P010 内核输出）、`e2e_ref.rgb` /
  `hdr_ref.rgb24`、`e2e_frame.yuv` / `hdr_frame.yuv`、`e2e_app.log` / `hdr_app.log`。
- 改动文件：`src/mpv/video/filter/yuv_to_rgba.c`（只动 limited 分支的跨度常量 +
  注释）。同批次的 `build/mpv/video/filter/yuv_to_rgba.c` 为构建树副本（`build_mpv.sh`
  会重新合并覆盖）。
- 仍未做：`vf_vsr.c:595`/`vf_vfg.c:409` 的临时 `yuv->rgb:` 诊断行保留（下次颜色
  排查还要用，定位完再删）。

---

## 20. 合成器的"内容峰值"：HDR10 静态元数据必须随声明下发（2026-10-17 续做）

### 20.1 症状与归因链

**症状**：同一帧 HDR10 彩条（`build/scratch/hdr_bars10s.mp4`，PQ/BT.2020，三条柱 = 纯
R/G/B 原色、PQ 码值 ≈1013/1023 ≈ 10000nit）

| 运行 | 看到的柱色 |
|---|---|
| `VSR_HDR=auto ./build/src/client/vsr-player`（默认声明） | **红 黄 青** |
| `mpv --vo=gpu-next --target-colorspace-hint=yes` | **红 绿 蓝** |
| `VSR_HDR=auto VSR_HDR_DECLARE=0 ./build/src/client/vsr-player` | **红 绿 蓝** |

归因（每条都有代码/协议/实测依据，**不是猜测**）：

1. **与解码/超分/内核无关**：`dump-both` 拿到的 VSR 输入（VO 之前）就是
   `(1013,0,0)/(0,1023,0)/(0,0,1010)`，与 ffmpeg 参考 mean|Δ|=0.98（§19）。
2. **mpv 根本不声明**：`PL_API_VER=360`（libplacebo 7.360.1）时
   `video/out/vulkan/context.c:554` 的 `bool supported = PL_API_VER >= 361 || !waylandvk;`
   为 **false** → `sw->fns->set_color()` 直接返回 false → `vo_wayland_handle_color()`
   从不执行。`WAYLAND_DEBUG=1` 实测 mpv：只发 `get_surface_feedback` /
   `get_preferred` / `get_information`，`create_parametric_creator` 与
   `set_image_description` **各 0 次**。mpv 只靠 VkColorSpace + 自己的 libplacebo 管线。
3. **我们的 VO 本地不做 tone mapping**：`target-trc=pq` + `target-peak=auto`
   → `video/out/gpu/video.c:2858` 把 `dst.hdr.max_luma` 取成
   `pl_color_transfer_nominal_peak(PQ) * MP_REF_WHITE = 10000/203*203 = 10000 nit`，
   src ≤ 10000 → 不触发。PQ 信号原样交给合成器。
4. **协议语义**（`/usr/share/wayland-protocols/staging/color-management/color-management-v1.xml`）：
   `set_luminances()` 描述的是**主色体积**，且 "With transfer_function.st2084_pq the
   given 'max_lum' value is **ignored**, and 'max_lum' is taken as 'min_lum' + 10000 cd/m²"
   —— 所以 `max_lum` **不是内容峰值**；内容峰值只能靠
   `set_mastering_luminance()`（ST 2086）与 `set_max_cll()` / `set_max_fall()`
   （CTA-861-H）表达。**我们原来这三个字段一个都没发。**
5. **KWin 的实现**（`src/core/colorpipeline.cpp`）：tone mapper 的输入峰值
   `maxWhiteLuminance` 由 `from->maxHdrLuminance().value_or(referenceLuminance)` 推出；
   而 wayland 后端的 image-description 解析（`1c320a86` 的
   `ImageDescriptionListener::done`）给出
   `maxHdrLuminance = max_cll → mastering max → 主色体积上限`。
   → 缺元数据时合成器只能假设"内容可达 10000nit"，对 400nit 面板做**最狠的压缩**。
6. **实测两素材的元数据**（mpv `video-params`）：

   | 素材 | mastering min/max | MaxCLL / MaxFALL |
   |---|---|---|
   | `input/Fallout …HDR…mkv`（真实片源） | 0.0001 / **1000** nit | **758 / 277** |
   | `build/scratch/hdr_bars10s.mp4`（合成） | 0 / 10000 nit | 0 / 0（无 CLL） |

   → 真实片源我们被按 **10000nit** 压（而内容只到 1000nit）→ 高光被过度压缩、
   对比度变平，**这就是用户最初"mpv 对比度更好"的根因**；合成条则是"内容真到
   10000nit"，压得狠是**正确**的，mpv 那边的饱和来自它没声明（KWin 走未声明路径）。

### 20.2 修复（已实现，2026-10-17）

在 HDR10 声明里补内容静态元数据（mpv `set_color_management()` 同款语义）：

- `WaylandColorFeedback.{h,cpp}`：新增 `struct vsr_hdr_meta`；
  `vsr_color_fb_declare_pq(win, meta)` 在原 `set_luminances(0,10000,203)` 之后补
  - `set_mastering_luminance(min*10000, max)`（**门控 feature
    `set_mastering_display_primaries`**，协议如此规定；mpv 同款门控）；
  - `set_max_cll()` / `set_max_fall()`（无门控）；
  - 合法性过滤 = mpv `cm_metadata_sanitize()`：`min>=max` 整体丢弃；v1 合成器要求
    cll/fall ∈ (min,max]；`fall>cll` 时两者清零。
  - mastering **基色**暂不下发（mpv 属性面没暴露；容器基色已是 BT.2020，KWin 缺省按
    容器基色处理）。
- `PlayerViewModel`：解析 `video-params/{min-luma,max-luma,max-cll,max-fall}`
  （DOUBLE/INT64 都接）→ `hdrMeta()` + `videoHdrMetaChanged()`。
- `HdrOutput`：`ContentMeta` + `setContentMeta()` —— 元数据变化时，若**当前已应用
  HDR10 且 `VSR_HDR_DECLARE != 0`** 就**重新下发**描述（换片即更新）；`off` 策略不碰。
- `main.cpp`：接线 `videoHdrMetaChanged → hdr::setContentMeta()`。

### 20.3 验证（可复跑）

```bash
F="input/Fallout S02E02 2025 2160p AMZN WEB-DL HDR H 265 DDP5 1 Atmos-ADWeb.mkv"
VSR_HDR=auto WAYLAND_DEBUG=1 timeout 12 ./build/src/client/vsr-player --scale off "$F" \
    > /tmp/fo_wl.log 2>&1
grep -c 'create_parametric_creator(' /tmp/fo_wl.log      # 2（切 HDR10 一次 + 元数据到达后重下发）
grep -o 'set_mastering_luminance([^)]*)' /tmp/fo_wl.log  # set_mastering_luminance(1, 1000)
grep -o 'set_max_cll([^)]*)' /tmp/fo_wl.log              # set_max_cll(758)
grep -o 'set_max_fall([^)]*)' /tmp/fo_wl.log             # set_max_fall(277)
grep -aiE 'protocol error|invalid_luminance' /tmp/fo_wl.log   # 空
```
实测输出（本机）：上述 4 条全部符合；合成条（无 CLL）→ `set_mastering_luminance(0,10000)`
+ `set_max_cll(0)/set_max_fall(0)`，行为与修复前一致（该素材本来就到 10000nit）。

### 20.4 结论与边界

- **"红黄青"不是 bug**：10000nit 的 BT.2020 原色映射到 400nit 面板必然去饱和（KWin 的
  tone mapper 在 ICtCp 上压强度）。**修好后真实片源的对比度应明显改善**（用户目检项）。
- mpv 的"红绿蓝"是**未声明路径**的结果（且随 libplacebo ≥ 7.361 会变成与我们现在
  相同的声明）；`VSR_HDR_DECLARE=0` 只是复现那条路径，不是正确配置。
- ⚠ **HDR 抓屏不能当判据**：本机 `spectacle -a` 截 HDR 窗口得到的结果与目检**相反**
  （我们=纯 R/G/B、mpv hint=蓝柱带绿）→ KWin 抓屏对 HDR 面做了自己的映射。判 HDR
  观感只能目检，或看协议流量/日志与参考实现代码。
- 无 mastering 元数据的 HDR10 内容，合成器仍按主色体积上限（10000nit）——与
  libplacebo 的名义口径一致，**不改**（想改只能自己本地 tone map，见 §18.5-2）。

---

## 21. `VSR_HDR=hdr10` 强制模式的两个真 bug（2026-10-17 修复）

用户验收（元数据修复后）：真实片源"跟 mpv 差别不大了，甚至对比度更强"✔；
`VSR_HDR_DECLARE=0` → 红绿蓝（= 预期，mpv 的未声明路径）；
**`VSR_HDR=hdr10` 播 SDR 片源（goose_480p.webm）→ 颜色不正确** ✗（本轮修）；
`VSR_HDR=off` 播 HDR 片源 → 红绿蓝但发灰（= 预期，HDR→SDR tone map，见 §21.3）。

### 21.1 bug 1：模式没变就早退 → mpv target 钩子从未执行

`HdrOutput.cpp` 的 `reevaluate()` 原本 `if (st.haveApplied && want == st.applied) return;`。
`VSR_HDR=hdr10` 是"启动即 Hdr10"（`installPolicy(..., initialMode=Hdr10)` 把
`applied=Hdr10, haveApplied=true`）→ 每次决策都命中早退 →
`hooks.updateMpvTarget()` 与 `applyMode` 里的色彩描述下发**一次都没执行**：

- mpv 侧没设 `target-trc=pq`/`target-prim=bt.2020` → gl_video 的目标色彩空间
  = 源（SDR BT.709/BT.1886）→ **SDR 码值被原样写进 PQ 面** → PQ 解释 gamma 码值
  = 又暗又过饱和，即"颜色不正确"。
- 我们的 `wp_color_management` 描述也没下发（只有 Qt 的，缺 luminances/元数据）。

**修法**：模式未变时也要同步"钩子输入"（`syncTargets(Mode)`：updateMpvTarget +
HDR10 时设 colorSpace + 下发描述），并在首帧探测里补一次 `reevaluate("first-frame")`
（首帧后 `wl_surface` 才存在）。`syncTargets` 幂等，重复调用安全。

### 21.2 bug 2：color management surface 重复 → 协议错误、app 退出

强制模式原本在 `show()` **之前**设 `QSurfaceFormat::colorSpace(BT2020/ST2084)`：
Qt 在创建 platform window 时据此自己建了一个
`wp_color_management_surface_v1`；我们的模块（`WaylandColorFeedback`）随后又
`wp_color_manager_v1.get_surface()` → KWin 回
`wp_color_manager_v1#N: error 1: there's already a color management surface for this wl_surface`
→ **协议错误 = 连接断开，app 退出（exit 255）**。

⚠ 协议约束：**一个 `wl_surface` 只能有一个 color management surface**
（旧注释"允许多个"是错的，已改）。而我们需要自己拥有它（只有我们能发
luminances + 内容静态元数据）。

**修法**：强制模式**不在 show 前设 colorSpace**（只设 `_qt_sg_hdr_format` 属性，
swapchain 格式需要它）；colorSpace 与描述都在**首帧之后**由 `syncTargets()` 下发
（此时 platform window 已存在，Qt 不会再建 surface——自动切换路径一直如此，
实测无冲突）。

**验证**：`VSR_HDR=hdr10 --scale off input/goose_480p.webm` →
`mpv HDR target: HDR10 (PQ/BT.2020) target-trc/prim=pq/bt.2020`、`已声明 HDR10`、
**0 条协议错误**、进程存活到 timeout；自动路径（Fallout）回归无变化：仍
`mastering 0.0001-1000 nit + max_cll 758 / max_fall 277`。

### 21.3 两个"看起来像 bug 的预期行为"

- `VSR_HDR=auto VSR_HDR_DECLARE=0` 播 HDR 彩条 → 红绿蓝：这就是 **mpv 的路径**
  （未声明 → KWin 走未声明分支）。它是复现对照，不是推荐配置。
- `VSR_HDR=off` 播 HDR 彩条 → 红绿蓝但发灰：`off` = 从不切 HDR10，HDR 内容由
  gl_video **tone map 到 SDR 输出**（= 本应用 HDR 上屏前的历史行为）。
  校验口径：`mpv build/scratch/hdr_bars10s.mp4`（**不加** `--target-colorspace-hint`）
  同样是 HDR→SDR tone map，两者应接近。
- ⚠ 强制模式（`VSR_HDR=hdr10`）播 **SDR** 内容会**比 SDR 播放暗**：SDR 白在 PQ
  容器里落在 203nit（libplacebo 参考白），而本机 SDR 桌面的白是 400nit；我们没做
  反相 tone mapping。mpv 做"SDR→HDR"用 `--inverse-tone-mapping` +
  目标峰值（面板 400nit）。**未实现**，属可选增强。

---

## 22. OSD HDR 状态行 + `VSR_HDR=hdr10` 的定位（2026-10-17）

### 22.1 `VSR_HDR=hdr10` = 诊断/覆盖，不是观看模式

- 用途只有三类：**机制验收/回归**（不需要 HDR 片源就能拉起 swapchain+声明+composite 链路，
  或在 SDR/WCG-only 屏上验证合成器接不接受 HDR10 面）、**逃生舱**（auto 判据失灵时唯一入口：
  合成器把真 HDR 报成 bt709、或片源 HDR 但没打 `pq/hlg` tag）、**变量分离**
  （HDR→面板 的映射由 gl_video 还是合成器做——auto/off vs hdr10）。
- ⚠ **不是画质功能**：播 SDR 内容时 SDR 白落在 203nit（libplacebo 参考白），比 SDR 桌面的
  400nit 暗，且**没有反相 tone mapping**（mpv 的对应能力是 `--inverse-tone-mapping`
  + `target-peak`）。正常播放不要用它；auto 下 SDR 内容本来就不走 HDR10，不受影响。
- 该模式**不暴露到 UI**（仅 env），避免用户做出"内容/屏幕不匹配"的错配选择。

### 22.2 OSD 新增 `HDR` 状态行

- 数据源：`hdr::statusText()`（`HdrOutput`，**不翻译**，与 OSD 其余状态段一致）；
  由 `ApplyHooks.statusChanged` 在**模式/显示能力/内容元数据**任一变化时推给
  `PlayerViewModel::setHdrStatus()`（加锁：GUI 线程写、事件线程 `osdTextString()` 读）。
- 位置：`osdTextString()` 在 `Output` 之后、`VSR` 之前新增一行；标签 `HDR` 走 tr()。
- 三种形态（本机实测日志 `hdr status: ...`）：
  | 场景 | OSD 行 |
  |---|---|
  | SDR 片源 | `SDR · content SDR` |
  | HDR 片源 + HDR 屏（切换瞬间→稳定） | `SDR · content HDR not displayed (display unknown)` → `HDR10 · BT.2020/PQ · display 400nit · content 758nit` |
  | 强制 `VSR_HDR=hdr10` + SDR 片源 | `HDR10 · BT.2020/PQ · display 400nit · content SDR · (forced)` |
- 口径：`content Xnit` 优先用 **MaxCLL**（KWin 实际用的值），退回 mastering max；
  `display Xnit` 用合成器 preferred 的 `max_luma`（**物理峰值** 400nit，不是 203 参考白口径）。
- 查看方式：正常播放按 **Tab** 显示 OSD（`osdPoll` 只在可见时推送 `osd-msg1`）；
  `--benchmark` 启动即显示。无 UI 核对用日志：`hdr status: ...`（仅在文本变化时打，INFO 级）。
- ⚠ **"从播放器内播放"（无参数启动 → 恢复播放列表 → 点播放/上次记录续播）实测同样正确**，
  但有顺序坑（2026-10-17 用户提问后实测并修）：切 HDR10 与 `video-params` 静态元数据
  到达可能是**同一瞬间**，`statusChanged` 先于元数据跑 → 文本会停在 `content ?`。
  修法：`setContentMeta()` 里元数据变化后**立即**刷新状态文本（先刷新再走早退），
  且内容 HDR 而元数据未到时显示 `content HDR`（不再显示 `?`）。实测序列：
  `… · content HDR` → `… · content 758nit`（毫秒级）。
  （命令行给文件那次只是碰巧元数据先到，所以没暴露这个 bug。）
