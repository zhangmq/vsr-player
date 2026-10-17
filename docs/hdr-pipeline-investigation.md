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
