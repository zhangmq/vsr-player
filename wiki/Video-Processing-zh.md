# 视频处理

> [English](Video-Processing) · 中文

NVIDIA Video Effects SDK 提供的全部能力都接进了 mpv 内的一条 GPU 滤镜链：

- **AI 超分与降噪** —— 基于 Tensor Core 的实时 2×/3×/4× 超分（`vf_vsr`），外加一遍降噪
  （Low…Ultra），降噪在 scale=1 时也能单独使用
- **AI 插帧** —— VFX `VideoFrameGeneration` 整数倍率 ×2/×3/×4，画质模式可热切换（`vf_vfg`）
- **HDR** —— 整条链端到端保持 **10-bit RGB10A2**（`cuda[x2bgr10]`），HDR 与 10-bit SDR 片源在
  进入 AI 环节之前不会被压成 8-bit；**HDR10 输出**在 HDR 内容遇到支持 HDR 的显示器时自动启用，
  **SDR→HDR10 上变换**（VFX TrueHDR，`vf_truehdr`）则放在一个需显式开启的开关后面
- **冻结的遗留后端** —— RIFE（TensorRT）插帧（`vf_rife`）只作为**源码构建选项**保留
  （`-Dfruc_backend=rife`）：不提供预编译包，也不再继续开发

## 快速切换处理环节

| 快捷键 | 环节 |
|---|---|
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` / `Ctrl+4` / `Ctrl+0` | 超分：关 / 2× / 3× / 4× / auto |
| `Ctrl+Shift+1` … `Ctrl+Shift+4` | 插帧：关 / 2× / 3× / 4× |
| `Ctrl+Shift+0` | 循环插帧的输出帧率上限：48 → 60 → 120 fps |
| `Ctrl+H` | SDR→HDR10 上变换开 / 关 |

每次变更都会在画面底部闪出一条简短的瞬态提示（3D 会话中每只眼各一条），下面的 OSD 行会显示结果。如果
AI 处理栈不可用，瞬态提示会如实说明，而不是假装切换成功。

## 阅读 OSD

三个 VFX 行格式一致 —— `<setting> | <what the filter reports>`：

```
VSR     2× High   | 2560×1440 mode=active scale=2.00 cost=2.8ms fps=30
TrueHDR on 400nit | mode=passthrough          ← HDR content, nothing to upconvert
FRUC    2× Medium | mode=active src=30.00 k=2 out=60.00 cost=0.7ms
```

- `mode=active` 总带有每帧耗时；`mode=passthrough reason=…` 只在环节已启用却有意闲置时出现（插帧最
  常见的原因是 `max-fps`，即 `k × source fps` 超过上限）；
- 关闭的环节只打印 `off` —— 不会有多余的直通信息；
- `Source` 带内容侧的 HDR 状态（内容峰值、是否执行上变换），`Output` 带真实输出模式
  （`HDR10 · BT.2020/PQ · display 380nit` 或 `SDR`）。

## 显存或驱动出问题时

某个 VFX 环节因 CUDA 显存不足或驱动错误而失败时，播放会停止并弹出提示卡说明情况，而不是静默绕过 AI
环节：显存不足的卡片列出两种可能原因（VFX 设置对该 GPU 过高，或应用存在显存泄漏 —— 请上报），驱动错误的
卡片则指向日志。释放显存（或调低设置）后按播放即可继续。

## 滤镜链

```
demux → decode → [deint] → [vf_hwup] → [vf_eye] → [vf_rot] → [vf_vsr] → [vf_truehdr] → [vf_vfg] → VO (libmpv) → Qt scene graph
                              ↑           ↑          ↑          ↑              ↑             ↑
                       SW→CUDA upload  single eye  rotate   VSR + denoise   TrueHDR        VFG
                                                                          (SDR→HDR10)   (VFX SDK)
```

这个顺序是出于成本的取舍，而非口味 —— 每个环节都放在按**源帧**计费一次、而不是按输出帧计费的位置：

- **`vf_hwup` 放在最前**：软解帧只上传到 CUDA 一次，后续每个环节看到的都是硬件帧（解码本身就是硬件时
  它是无操作直通）。
- **`vf_vsr` 在 `vf_vfg` 之前**：放在插帧*之后*的每个环节都要按 ×2/×3/×4 计费，而 VSR 正是昂贵的那
  一环（实测：1080p→4K 一帧约 8–9 ms；同为 4K 时 VFG 约 3 ms、TrueHDR 约 5 ms）。因此超分在小画面上按
  源帧各跑一次，插帧放在最后、作用于已完成处理的画面。`--fruc-order pre` 把 VFG 放到 VSR 前面 ——
  那会让 VSR 的账单再乘上插帧倍率，所以默认是 `post`，`pre` 只用于实验。
- **`vf_eye` 在 `vf_vsr` 之前**：单眼输出（见 [3D 与单眼](3D-and-Single-Eye)）先裁出单眼，超分因此
  作用在单眼画面上，而不是左右并排的整帧。
- **`vf_rot` 在 `vf_vsr` 之前**：旋转只是纯粹的像素搬运；放在超分之前做，超分看到的就是最终方向
  （其 auto 倍率也按旋转后的画面计算）。
- **`vf_truehdr` 位于 VSR 与 VFG 之间**：放在 VSR 之后，是为了让 VSR 仍面对它所调校和验证过的
  SDR 输入分布；放在 VFG 之前，是为了让上变换同样按源帧只跑一次。VFG 的 10-bit 通路随后原生插值
  HDR10 帧。
- **`vf_truehdr` 需显式开启，且有自我保护**：它只对 SDR 内容运行（滤镜会重新检查帧的传输函数，HDR
  内容直接直通），且只在开关打开时运行；若显示器处于 SDR 模式，输出保持 SDR，由 mpv 把上变换后的帧
  再色调映射下来。
- **RIFE 构建**使用另一套固定顺序 —— `hwup → rife → vsr`（在源分辨率上插帧，再超分），`truehdr`
  位于链尾。已冻结：仅源码构建。

支撑部分：

- 解复用、解码、A/V 同步、时序、seek 与 VO 都由 mpv 负责。
- `vf_vsr` 接收一个 `mp_image` 并输出超分后的 `mp_image`；YUV→RGB 转换是自研的 NVRTC CUDA 内核
  （NV12/P010/P016 → RGBA8 或打包 RGB10A2，全程 D2D）。
- 前端：Qt 6 + QML（`src/client/`）—— `MpvController`（libmpv 封装）、`PlayerViewModel`
  （唯一事实源）、与 mpv 共享的 Vulkan 设备。
- mpv 补丁机制：`third_party/mpv`（未改动的 0.42.0）+ `src/mpv` overlay，由
  `scripts/build_mpv.sh` 合并。

## 输出段与 HDR

`vo=libmpv` 渲染到与 Qt 场景图共享的 Vulkan 交换链。其格式与色彩空间声明取决于*内容 × 显示能力*：

- 默认 8-bit SDR；
- **HDR10**（PQ/BT.2020，10-bit，带内容的静态元数据）—— 当 HDR 内容遇到支持 HDR 的显示器时；
- 开启上变换时则是 SDR→HDR10 的情形。

切换在运行期完成（切换播放列表、显示器变化），无需重启。OSD 的 `Output` 行报告的是**实际**的交换链
格式，而非预期意图。

在 Linux 上无法从 Qt 读取显示能力（`QRhiSwapChain::hdrInfo()` 是硬编码的默认值；`isFormatSupported()`
只报告合成器整体的支持情况）。唯一可靠的来源是通过 `wp_color_management_v1` 拿到的 surface preferred
描述（`gamut_area` / 传输函数）；读不到时，播放器会有意保持在 SDR。

## 自动缩放的 3D 片源

`scale=auto` 时，倍率按**单眼**计算 —— 公式与实测见 [3D 与单眼](3D-and-Single-Eye)。

## 竖屏视频与插帧画质档位

NVIDIA 的 VFG 引擎对**竖屏 / 窄幅**输入会走一条明显更慢的内部路径（这是 VFX SDK 1.3 的行为，不是
本项目的代码）：

| 输入（`--fruc-quality medium`） | 每次插值的引擎耗时 |
|---|---|
| 1920×1080 横屏 | ≈2.5 ms |
| 3840×2160 横屏 | ≈3.4 ms |
| 1920×3328 竖屏 | ≈15–28 ms |

竖屏下档位顺序还会反转：同样是 1920×3328 的输入，LOW ≈4 ms、**MEDIUM ≈15–28 ms**、**HIGH ≈5.5 ms**。
因此竖屏内容请在画质弹出面板的「Interpolation quality」一行里选 **High**（更快*且*更好看），或者选
Low；**避开 Medium**。同一段竖屏素材（30 fps ×2）在播放器内的数字：MEDIUM 每输出帧约 15 ms，
HIGH 约 5.5 ms，LOW 约 4 ms。

可用 `tests/vfg/vfg_timing_probe.c` 复现（一个纯 NvVFX 探针，不涉及播放器）。
