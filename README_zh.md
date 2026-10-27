# VSR Player

Linux 桌面实时 AI 视频处理播放器，直接调用 **NVIDIA Video Effects SDK**：超分 + 降噪、插帧，以及 10-bit HDR 通路（HDR10 上屏、SDR→HDR10 上变换）。基于 **libmpv**——解封装、解码、音视频同步、VO 渲染全部交给 mpv；前端是 Qt 6 + QML，与 mpv 共享 Vulkan 设备。

> NVIDIA 的驱动级 RTX VSR 在 Linux 上没有接口。Video Effects SDK 提供了同一批模型和 Linux 运行时，所以本项目在播放器里直接调用它的 C API。这不是一条"合理"的路线——这类处理本该在驱动或合成器层——它只是驱动级路径出现之前的变通方案。

## 能做什么

- **AI 超分 + 降噪** — 实时 2×/3×/4×（`vf_vsr`），降噪在 scale=1 时也能单独生效
- **AI 插帧** — VFX `VideoFrameGeneration` ×2/×3/×4，质量档可热切换（`vf_vfg`）
- **10-bit / HDR 全链** — RGB10A2 贯通整条链；HDR 内容自动切 HDR10 上屏；SDR→HDR10 上变换为 opt-in（`vf_truehdr`）
- **眼镜 3D** — 左右/上下并排片源，每眼半幅，输出到 3840×1080 级 framebuffer
- **单眼 2D 输出** — 只看 3D 片源的一只眼，按正常画幅满屏播放，且超分按单眼算（`vf_eye`）
- **蓝光 ISO** — `.iso` 当作文件夹/m3u 处理：列出各 title 的名字与时长，不自动播放
- **日常播放** — NVDEC 硬解、Vulkan 渲染、自动隐藏 QML UI、播放列表、循环、倍速、截图、JSON IPC、独立 `mpv-vsr` CLI

## 最近新增

| 功能 | 说明 |
|---|---|
| **单眼 2D 输出** | 新增 `vf_eye` 滤镜，在**超分之前**取出单眼，因此 VSR 按单眼画面工作。半幅片源由滤镜内部拉伸回自然尺寸，所以画面比例、旋转、自动倍率全都按普通 2D 处理 |
| **3D 菜单分组** | 右键菜单新增 **3D** 小块，三条**互相独立**的入口：**输出方式**（关闭 / 眼镜 3D / 2D 输出）、**片源格式**（左右或上下，全幅或半幅）、**输出选项**（左眼 / 右眼） |
| **画面比例与 mpv 对齐** | 底栏弹窗与新增的 `A` 快捷键循环 mpv 自带的那组：`16:9`、`4:3`、`2.35:1`、`no`。3D 下该值表示**每眼**比例，下发前按布局换算 |
| **「画面处理」分组** | 反交错与旋转归到一个分组标题下 |
| **蓝光 title 命名** | 播放列表显示 `Title 6 · 1:18:30`，不再是 ISO 文件名——重启后也一样 |
| **播放列表快照** | 列表持久化为 m3u8，恢复时交给 mpv 自己的播放列表解析器，条目名因此能跨重启保留 |
| **启动保持 idle** | 无参数启动只还原列表：不解码、不加载引擎、不占显存，按下播放才开始 |

## 截图

![播放器界面](docs/images/player-screenshot.jpg)

**VSR 对比** —— 原始 720p 与同一帧 4× 超分：

![原始 720p](docs/images/00003_orig.jpg)
![VSR 4× 超分](docs/images/00003_vsr.jpg)

## 快速开始

**源码构建**（推荐——二进制按构造匹配你的系统库）：

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# 先按 docs/third-party-setup.md 准备 third_party/
./scripts/build-from-source.sh
```

**预编译包**（仅 `standard`；需要 Qt ≥ 6.12、Vulkan loader、NVIDIA 驱动）：

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                 # 装到 ~/.local，不需要 sudo
```

两种方式都会装上 **VSR Player** 启动器入口，可设为视频/音频默认打开方式。打包变体、手工构建与版本兼容性见 [wiki：安装](https://github.com/zhangmq/vsr-player/wiki/Installation)。

## 前置要求

| | 最低 | 说明 |
|---|---|---|
| GPU | RTX 20 系 | 超分需 RTX 20+；默认插帧后端需 **Ada/Blackwell**（冻结的 RIFE 源码构建需 Ampere+） |
| 驱动 | 570+ | Wayland 开箱可用 |
| VFX SDK | 1.3（`nvidia-vfx` 0.2.0.0） | `install.sh` 从 NVIDIA 官方索引获取，不进任何 Python 环境 |
| Qt | 6.12+ | 否则自行构建 `full` 变体 |

## 播放

```bash
vsr-player video.mkv            # 单个文件
vsr-player ~/Videos             # 目录 → 播放列表
vsr-player                      # 还原上次列表，按下播放才载入
```

| 键 | 作用 |
|---|---|
| `Space` | 播放 / 暂停 |
| `←` / `→` | 快退/快进 5 秒（`Shift` = 10 秒） |
| `↑` / `↓` | 音量 ±5% |
| `A` / `Shift+A` | 循环画面比例 |
| `P` / `F` / `Tab` / `S` | 播放列表 · 全屏 · OSD · 截图 |
| `N` / `B` | 下一个 / 上一个 |
| `J` / `K` / `L` | 3D 或单眼模式：布局 · 眼序 · 全半 |

右键菜单里有画质、**画面处理**（反交错、旋转）和整个 3D 分组。完整选项、JSON IPC 命令与独立 `mpv-vsr` CLI 见 [wiki：使用](https://github.com/zhangmq/vsr-player/wiki/Usage)。

## 文档

- **Wiki** — [安装](https://github.com/zhangmq/vsr-player/wiki/Installation) ·
  [使用](https://github.com/zhangmq/vsr-player/wiki/Usage) ·
  [视频处理](https://github.com/zhangmq/vsr-player/wiki/Video-Processing) ·
  [3D 与单眼输出](https://github.com/zhangmq/vsr-player/wiki/3D-and-Single-Eye) ·
  [蓝光与播放列表](https://github.com/zhangmq/vsr-player/wiki/Blu-ray-and-Playlists) ·
  [疑难排查](https://github.com/zhangmq/vsr-player/wiki/Troubleshooting) ·
  [开发](https://github.com/zhangmq/vsr-player/wiki/Development)
- [`docs/`](docs) — 工程记录与实测数据（HDR 管线、VFX SDK、TrueHDR、3D 转换……）
- [`docs/third-party-setup.md`](docs/third-party-setup.md) — 源码构建的 `third_party/` 准备

## 现状与范围

视频处理这一侧在设计范围内已经做完。其余部分——窗口外观、控件布局、热区行为、OSD 文案——是作者个人喜好而非规范，**看心情改**，本 README 不是路线图。

本项目在有限硬件上开发（单一 GPU 世代、少量测试样本），边角问题请有预期：崩溃、画质异常或卡死都可能出现在未覆盖的组合上。欢迎 issue 与 PR。仓库刻意对 AI 代理友好：代码、mpv 补丁覆盖层与 `docs/` 里的设计记录，足以让代理快速理解管线并定位问题。

## 许可

GPLv2+。项目编译了打过补丁的 mpv 并链接 libmpv，因此分发的二进制是 mpv 的衍生作品，以同一许可分发。NVIDIA Video Effects SDK 运行时是 NVIDIA 专有软件，不受本项目许可约束。详见 [LICENSE](LICENSE)。

---

[English](README.md)
