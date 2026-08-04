# VSR Player

Linux 桌面实时 AI 超分辨率视频播放器。使用 NVIDIA Video Effects SDK 在视频播放过程中进行神经超分和降噪。

基于 **libmpv**——解封装、解码、音视频同步、时序和 VO 渲染全部由 mpv 承担。VSR 作为自定义 mpv 视频滤镜（`vf_vsr`）注入，通过 CUDA + Video Effects SDK 超分后送回 mpv 管线。前端为 Qt 6 + QML 客户端（Vulkan，与 mpv 共享设备）。

## 背景

NVIDIA RTX Video Super Resolution（RTX VSR）在 Windows 上已经可用了一段时间，通过驱动集成，浏览器和主流播放器都能直接调用。但在 Linux 上，这个驱动级接口并未开放，mpv、VLC 等播放器目前都无法使用 RTX VSR。

NVIDIA Video Effects SDK 提供了相同的底层 AI 模型，也有 Linux 版本，但它并不是一个拿来就能用的依赖——目前还是 Early Access 状态，附带约 1 GB 的推理运行时，也没有现成的播放器集成路径。

这个项目直接调用了 Video Effects SDK 的 C API，将其接入一个播放器中。这不是一条特别合理的路线——这类处理逻辑理应放在驱动或合成器层面——只是在驱动级 VSR 尚未支持 Linux 的情况下的一个变通方案。

## 特性

- **AI 超分辨率** — Tensor Cores 实时 2×/3×/4× 超分（mpv 视频滤镜 `vf_vsr`）
- **AI 降噪** — 可配置降噪强度（低至超高），scale=1 时单独生效
- **NVDEC 硬解码** — AV1、H.264、HEVC GPU 解码（含软解回退）
- **Vulkan 渲染** — CUDA-Vulkan 共享设备，mpv 渲染进 Qt 场景图
- **QML 叠加 UI** — 底部悬停区域驱动的自动隐藏控件、虚拟化播放列表、OSD 信息面板
- **自适应缩放** — 按视口尺寸自动选择超分倍率
- **播放列表与播放控制** — 目录加载、循环模式、倍速、音视频同步（mpv 承载）
- **远程控制** — Unix socket JSON IPC；独立 `mpv-vsr` CLI 与包装脚本

## 截图

![播放器界面](docs/images/player-screenshot.jpg)

### VSR 对比

**原始画面（720p）**

![原始 720p 帧](docs/images/00003_orig.jpg)

**VSR 4× 超分**

![VSR 4x 超分帧](docs/images/00003_vsr.jpg)

## 架构

```
demux → decode → [vf_vsr] → VO (libmpv) → Qt 场景图
                   ↑
              VFX SDK + CUDA
```

- mpv 承担：解封装、解码、音视频同步、时序、seek、VO
- `vf_vsr`（覆盖层 `src/mpv/video/filter/vf_vsr.c`）：接收 `mp_image`，CUDA+VFX SDK 超分，输出超分 `mp_image`
- 前端：Qt 6 + QML（`src/client/`）——MpvController（libmpv 封装）、PlayerViewModel（UI 状态单一事实源）、Vulkan 共享设备
- mpv patch 方案：`third_party/mpv`（纯净 0.41）+ `src/mpv` 覆盖层，`scripts/build_mpv.sh` 合并构建

## 前置要求

| 组件 | 要求 |
|------|------|
| GPU | NVIDIA RTX 20 系或更新 |
| 驱动 | 570+（含 CUDA；Wayland 需 `nvidia_drm.modeset=1`） |
| Qt | 6.8+（Quick、QuickControls、Vulkan） |
| 编译器 | GCC 13+（C++20） |
| 构建 | meson、ninja、CUDA Toolkit（`/opt/cuda`） |

第三方 SDK（NvVFX 头文件/运行时、MDI 图标字体、mpv 源码）**不随仓库分发**——按 [docs/third-party-setup.md](docs/third-party-setup.md) 准备 `third_party/`。

## 构建

```bash
./scripts/build_mpv.sh          # 合并 src/mpv 覆盖层到 third_party/mpv 并构建 libmpv + vf_vsr
ninja -C build                  # 构建 Qt 客户端
./build/src/client/vsr-player <视频或目录>
```

## 使用

- 打开文件或目录（目录会把全部可播放文件加载进播放列表）
- 画质控制：底栏 `画质` 弹窗——缩放 off/auto/2×/3×/4×、超分质量、降噪
- 播放列表：`P` 键；UI 自动隐藏由底部悬停区域驱动（鼠标移出即隐藏）
- OSD：`Tab` 切换 mpv 渲染的信息浮层（源/输出/渲染/解码器/GPU…）

### 命令行选项

| 选项 | 取值 | 默认 | 说明 |
|------|------|------|------|
| `--scale` | `off`、`auto`、`2`、`3`、`4` | `auto` | 超分倍率 |
| `--quality` | `low`、`medium`、`high`、`ultra` | `high` | 超分质量 |
| `--denoise` | `off`、`low`、`medium`、`high`、`ultra` | `off` | 降噪强度（scale=1 时生效） |
| `--no-hwaccel` | — | — | 关闭 NVDEC，使用软解 |
| `--lang` | 如 `en`、`zh_CN` | 系统 locale | 界面语言 |
| `--benchmark` | — | — | 无 UI 吞吐测量（`all=no` 日志） |
| `--no-vsync` | — | — | 关闭 FIFO present |
| `--no-rpc` | — | — | 关闭 JSON IPC 服务 |

### 快捷键

| 按键 | 动作 |
|------|------|
| `Space` | 播放 / 暂停 |
| `←` / `→` | 快退 / 快进 ±5s（`Shift` + 方向键 = ±10s） |
| `↑` / `↓` | 音量 ±5% |
| `S` | 截图 |
| `Tab` | 切换 OSD |
| `N` / `B` | 下一首 / 上一首 |
| `[` / `]` / `\` | 倍速 0.5× / 2× / 1× |
| `P` | 开关播放列表 |
| `F` | 切换全屏 |
| `Esc` | 退出全屏 / 关闭播放列表 |

## 远程控制

Unix socket JSON IPC（`/tmp/vsr-player.sock`）：

```bash
printf '{"command":["play"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

命令：`play`、`pause`、`stop`、`seek`、`loadfile`、`set-vsr`、`get-vsr`、`quit`，及原始 `command` 透传。独立 CLI（`scripts/install_mpv_local.sh`）安装 `mpv-vsr`——带 `--vf=vsr` 支持的 patched mpv。

## 独立版 mpv-vsr CLI

带 `vf_vsr` 滤镜的 patched mpv 也独立构建分发，可作普通 mpv 替代：

```bash
./scripts/install_mpv_local.sh     # → ~/.local/bin/mpv-vsr + VFX 库在 ~/.local/lib/vsr-player/
mpv-vsr --vf=vsr:scale=2 video.mkv
```

滤镜选项：

| 选项 | 取值 | 说明 |
|------|------|------|
| `scale` | `off`、`auto`、`2`、`3`、`4`、比例（如 `4/3`） | 超分倍率；`auto` 按视口选择 |
| `quality` | `low`、`medium`、`high`、`ultra` | VSR 推理质量 |
| `denoise` | `off`、`low`、`medium`、`high`、`ultra` | 降噪（scale=1 时单独生效） |

示例：`mpv-vsr --vf=vsr:scale=auto,quality=ultra --hwdec=auto video.mkv`

`mpv-vsr-wrapper.py`（浏览器集成，ff2mpv 风格）：导出 Chrome cookie、yt-dlp 提取播放列表（YouTube/Bilibili/Niconico）、拉起 mpv-vsr。

## 开源协议

VSR Player 采用 GNU General Public License v2 或更高版本 (GPLv2+)。

完整协议文本见 [LICENSE](LICENSE)。

项目基于 [mpv](https://github.com/mpv-player/mpv)（GPLv2+）构建：编译带自定义
`vf_vsr` 滤镜的 patched mpv，前端链接 libmpv——因此分发的二进制是 mpv 的
衍生作品，整个项目按 GPLv2+ 分发。Qt/QML 前端代码本身为独立开发。

运行时动态加载的 NVIDIA Video Effects SDK 库为 NVIDIA 专有软件，
不受本项目 GPL 许可证约束。

---

[English](README.md)
