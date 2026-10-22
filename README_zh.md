# VSR Player

Linux 桌面实时 AI 视频处理播放器，直接调用 NVIDIA Video Effects SDK：**超分 + 降噪**、**插帧**与 **HDR**（10-bit RGB10A2 通路、HDR10 上屏、SDR→HDR10 上变换）。

基于 **libmpv**——解封装、解码、音视频同步、时序和 VO 渲染全部由 mpv 承担。AI 环节以自定义 mpv 视频滤镜（`vf_vsr`、`vf_vfg`、`vf_truehdr`）注入，在 CUDA 上运行后把结果送回 mpv 管线。前端为 Qt 6 + QML 客户端（Vulkan，与 mpv 共享设备）。

## 背景

NVIDIA RTX Video Super Resolution（RTX VSR）在 Windows 上已经可用了一段时间，通过驱动集成，浏览器和主流播放器都能直接调用。但在 Linux 上，这个驱动级接口并未开放，mpv、VLC 等播放器目前都无法使用 RTX VSR。

NVIDIA Video Effects SDK 提供了相同的底层 AI 模型，也有 Linux 版本，但它并不是一个拿来就能用的依赖——目前还是 Early Access 状态，附带约 1 GB 的推理运行时，也没有现成的播放器集成路径。

这个项目直接调用了 Video Effects SDK 的 C API，将其接入一个播放器中。这不是一条特别合理的路线——这类处理逻辑理应放在驱动或合成器层面——只是在驱动级 VSR 尚未支持 Linux 的情况下的一个变通方案。

## 视频处理能力（NVIDIA Video Effects SDK）

本项目计划覆盖的三项 VFX SDK 能力**都已接通**，并且能组合进同一条 GPU 滤镜链：

- **AI 超分 + 降噪** — Tensor Cores 上实时 2×/3×/4× 超分（`vf_vsr`），另有一路降噪（低至超高），scale=1 时也能单独生效
- **AI 插帧（FRUC）** — VFX `VideoFrameGeneration`（VFG）整数倍率 ×2/×3/×4，质量档 `low`/`medium`/`high` 可热切换（`vf_vfg`）
- **HDR** — 全链走 **10-bit RGB10A2**（`cuda[x2bgr10]`），HDR 与 10-bit SDR 片源不会在进 AI 之前被压成 8-bit；**HDR10 上屏**在「HDR 内容 × 支持 HDR 的显示」时自动生效（运行期切换、无需重启）；**SDR→HDR10 上变换**（VFX TrueHDR，`vf_truehdr`）放在一个 opt-in 开关后面
- **冻结的旧后端** — RIFE（TensorRT）插帧（`vf_rife`，目标帧率 40/48/60）**仅**保留为源码构建选项（`-Dfruc_backend=rife`）：不再出预编译包，也不再投入

## 播放器

- **NVDEC 硬解码** — AV1、H.264、HEVC GPU 解码（含软解回退；软解帧经 `vf_hwup` 上传为 CUDA 帧）
- **Vulkan 渲染** — CUDA-Vulkan 共享设备，mpv 渲染进 Qt 场景图
- **QML 叠加 UI** — 底部悬停区域驱动的自动隐藏控件、虚拟化播放列表、OSD 信息面板
- **自适应缩放** — 按视口尺寸自动选择超分倍率
- **播放列表与播放控制** — 目录加载、循环模式、倍速、音视频同步（mpv 承载）
- **远程控制** — Unix socket JSON IPC；独立 `mpv-vsr` CLI
- **桌面集成** — 应用菜单/启动器入口（随包分发图标、可设为视频/音频默认打开方式）

## 现状与范围

**视频处理这一侧在设计范围内已经做完**：超分、插帧、HDR 都由下面的管线覆盖，不再规划新能力。其余部分——窗口外观、控件布局、热区行为、OSD 文案——是作者的个人喜好而非规范：**看心情改**，本 README 不是路线图。

本项目在有限的硬件条件下开发——单一 GPU 世代、少量测试样本，无法覆盖所有 GPU/驱动/媒体组合。请对边角问题有预期：你可能会遇到这里从未见过的崩溃、画质异常或卡死。

遇到问题时，一个高效路径是让 AI 编码代理协助排查。本项目本身就是用 AI 代理开发的（Claude Code 驱动 DeepSeek 模型，低成本方案，无需担心成本）：代码、mpv 补丁覆盖层、`docs/` 与提交历史里的设计记录一应俱全，代理可以快速理解管线并定位修复。也欢迎提交 issue 和 PR。

## 截图

![播放器界面](docs/images/player-screenshot.jpg)

### VSR 对比

**原始画面（720p）**

![原始 720p 帧](docs/images/00003_orig.jpg)

**VSR 4× 超分**

![VSR 4x 超分帧](docs/images/00003_vsr.jpg)

## 管线

```
demux → decode → [vf_hwup] → [vf_vsr] → [vf_truehdr] → [vf_vfg] → VO (libmpv) → Qt 场景图
                 ↑            ↑            ↑              ↑
            软解→CUDA 上传  超分 + 降噪    TrueHDR         VFG
                           (VFX SDK)     (SDR→HDR10)     (VFX SDK)
```

顺序是**成本决策**，不是口味问题——每个环节都放在「按**源帧**计费一次」而不是「按输出帧计费一次」的位置：

- **`vf_hwup` 放最前**：软解帧只上传 CUDA 一次，下游环节永远只处理硬件帧（硬解时是空操作）。
- **`vf_vsr` 在 `vf_vfg` 之前**：插帧**之后**的环节按 ×2/×3/×4 计费，而超分是最贵的环节（实测：1080p→4K 约 8–9 ms，4K 下 VFG 约 3 ms、TrueHDR 约 5 ms）。所以超分按源帧在小画面上做一次，插帧放最后、对处理完的画面做。`--fruc-order pre` 把 VFG 提到 VSR 之前，等于把超分账单乘以插帧倍率——所以默认是 `post`，`pre` 只用于实验。
- **`vf_truehdr` 夹在两者之间**：放在 VSR 之后，超分的输入保持它被调校与验证过的 SDR 分布；放在 VFG 之前，上变换同样按源帧只做一次，而不是每个插值帧都做一次。VFG 的 10-bit 通路随后原生插值 HDR10 帧。
- **`vf_truehdr` 是 opt-in 且有自守卫**：只对 SDR 内容做上变换（滤镜会再按帧的 transfer 判断一次，HDR 内容直通），且只在开关打开时生效；面板处于 SDR 档时输出仍是 SDR，由 mpv 把上变换后的帧 tone map 回 SDR。
- **RIFE 构建**用另一套固定顺序——`hwup → rife → vsr`（先在源分辨率插帧，再超分），`truehdr` 在链尾。已冻结：仅源码构建。
- **输出端**：`vo=libmpv` 渲染进与 Qt 场景图共享的 Vulkan swapchain。面的格式与色彩空间声明都按「**内容 × 显示能力**」决定：默认 8-bit SDR；HDR 内容遇到支持 HDR 的显示时切 HDR10（PQ/BT.2020、10-bit、带内容的静态元数据）；上变换打开时是第三种情形。切换发生在运行期（换片、换屏），无需重启。

支撑部分：

- mpv 承担：解封装、解码、音视频同步、时序、seek、VO
- `vf_vsr`（覆盖层 `src/mpv/video/filter/vf_vsr.c`）接收 `mp_image`、输出超分后的 `mp_image`；YUV→RGB 转换是自研 NVRTC CUDA 内核（NV12/P010/P016 → RGBA8 或打包 RGB10A2，全程设备内）
- 前端：Qt 6 + QML（`src/client/`）——MpvController（libmpv 封装）、PlayerViewModel（UI 状态单一事实源）、Vulkan 共享设备
- mpv patch 方案：`third_party/mpv`（纯净 0.41）+ `src/mpv` 覆盖层，`scripts/build_mpv.sh` 合并构建

## 前置要求

| 组件 | 要求 | 适用 |
|------|------|------|
| GPU | 超分需 NVIDIA RTX 20 系或更新；插帧默认走 VFG 后端，需 **Ada/Blackwell**（冻结的 RIFE 源码构建后端需 Ampere+ 且支持 FP16 Tensor Cores） | 全部 |
| 驱动 | 570+（含 CUDA；Wayland 开箱即用——现代驱动默认启用 DRM modeset，无需内核参数） | 全部 |
| VFX SDK | NVIDIA VFX SDK **1.3**（`nvidia-vfx` 0.2.0.0 wheel）——超分、插帧与 TrueHDR 都来自它；`install.sh` 从 NVIDIA 官方索引（`pypi.nvidia.com`）下载并解包，不装进 Python 环境 | 全部 |
| Qt | 6.12+（Quick、QuickControls、Vulkan） | 源码构建 / standard 包（不再分发预编译 full 包——需要就用 `--variant=full` 自建） |
| 编译器 | GCC 13+（C++20） | 源码构建 |
| 构建 | meson、ninja、CUDA Toolkit（`/opt/cuda`）；TensorRT（`trtexec`）仅构建冻结的 RIFE 后端时需要 | 源码构建 |

第三方 SDK（NvVFX 头文件/运行时、MDI 图标字体、mpv 源码）**不随仓库分发**——按 [docs/third-party-setup.md](docs/third-party-setup.md) 准备 `third_party/`（源码构建需要）。

## 安装

### 方式 A（推荐）：源码构建

直接在本机构建。产出的二进制**天然匹配本机系统库**——系统升级后重跑同一条命令即可恢复，不存在预编译包"系统 soname 一变就启动失败"的问题（本项目近期就踩过 libcdio/libbluray 升级导致启动失败的坑）。

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# 1) 按 docs/third-party-setup.md 准备 third_party/（NvVFX 运行时、MDI 字体、mpv 源码；RIFE ONNX 仅 -Dfruc_backend=rife 需要）
./scripts/build-from-source.sh
```

脚本一次做完：依赖自检 → 构建 libmpv + 客户端 → 安装到 `~/.local`（免 sudo）→ 记录依赖快照。首次会从 **NVIDIA 官方索引**自动下载 VFX 运行时（wheel 约 0.6 GB，解压后约 1.1 GB）；已有可加 `--no-vfx` 跳过，另有 `--no-install`、`--jobs N`。

**系统升级后**（Qt/FFmpeg/媒体库 soname 变化会让旧二进制启动失败）：

```bash
./scripts/check-deps.sh              # 看漂移提示（构建时快照 vs 当前系统库）
./scripts/build-from-source.sh       # 一条命令重建
```

### 方式 B：预编译包

不想自己编译，或发行版没有 FFmpeg 9.x 开发包时使用。

| | standard（唯一分发的预编译包） | full（仅源码自建——`--variant=full`，自带 Qt） |
|---|---|---|
| 自带 | libmpv + ffmpeg×7 + CUDA runtime + **光盘媒体栈**（libcdio/libdvd/libbluray）+ 字体/翻译/许可。**只带 VFG 后端**：不含 TensorRT 库（`libnvinfer*`）、不含 `engines/` | 同上 **+ Qt6（库/平台插件/QML 模块）** |
| 还需要系统提供 | **Qt ≥ 6.12**、Vulkan loader、Wayland/X11 客户端库、NVIDIA 驱动 | 仅 Vulkan loader、Wayland/X11 客户端库、NVIDIA 驱动 |
| 适用 | 主流滚动发行版（Arch/CachyOS/Fedora 等） | Qt < 6.12 的发行版（Debian stable、Ubuntu LTS 等）——用 `--variant=full` 自行构建 |
| 体积 | 约 56 MB | 约 78 MB（`--variant=full` 自建产物） |

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                         # 安装到 ~/.local，无需 sudo
```

**可选的 `-rife` 包已停发**（自 0.4.0 起）：RIFE 后端已冻结，不再提供预编译 tarball。它仍可
作为源码构建选项使用——`./scripts/build-from-source.sh --fruc-backend=rife`，或
`./scripts/build_release.sh --fruc-backend=rife` 自行打包——这对 Ampere 及更早的 GPU 有意义，
因为默认的 VFG 后端需要 Ada/Blackwell。

安装会一并加好**桌面集成**：应用菜单/启动器里出现 **VSR Player**（可固定到任务栏、可设为视频/音频文件的默认打开方式），图标随包分发（当前为占位图标，后续替换 `data/vsr-player.svg` 即可）。

判断用哪个：`pkg-config --modversion Qt6Quick`（≥ 6.12 → standard；否则用 `--variant=full` 自建——本项目不再分发预编译 full 包）。下限是 6.12 的原因：发布包用 **Qt 6.12.0** 编出，而 Qt 6 的二进制兼容是**单向**的（用 6.x 编的包只能在 6.x 及更新的 6.y 上跑，更旧的 6.y 无保证）。

- **VFX 运行时**（wheel 约 0.6 GB，解压后约 1.1 GB）**不随包分发**（NVIDIA SLA 禁止再分发）：`install.sh` 会从 **NVIDIA 官方索引 `pypi.nvidia.com`** 选最新 manylinux wheel 下载提取（curl 纯下载、不调 pip、不改系统），也可手动放置到 `~/.local/lib/vsr-player/`。需要的版本是 **NVIDIA VFX SDK 1.3**（`nvidia-vfx` 0.2.0.0）——超分、插帧与 TrueHDR 都来自它
- 打包采用"显式核心集 + 媒体栈动态发现 + **打包时闭包自检**"：既未捆绑、也不在系统白名单的依赖会让打包直接失败，不会带病发布
- 两个变体都还依赖发行版常规组件：Vulkan loader、Wayland/X11 客户端库，以及 mpv/FFmpeg 的可选依赖（libass、libplacebo、libmujs、liblcms2、libwebp、libopenjp2、1394 采集库等）——由闭包自检逐项校验

## 手工构建（开发/调试）

1. **准备 `third_party/`** — NvVFX SDK 头文件/运行时、MDI 图标字体、mpv 源码、CUDA 12 存档；RIFE ONNX 资产仅 `-Dfruc_backend=rife` 需要：按 [docs/third-party-setup.md](docs/third-party-setup.md) 操作。
2. **构建并运行：**

```bash
./scripts/build_mpv.sh          # 合并 src/mpv 覆盖层 → third_party/mpv，构建 libmpv + 滤镜
ninja -C build                  # 构建 Qt 客户端
./build/src/client/vsr-player <视频或目录>
```

**注意事项（都是踩过的坑）：**

- **mpv patch 方案**：`third_party/mpv` 是纯净基座；`src/mpv/` 是覆盖层（镜像 mpv 树，只含修改文件）。改完 `src/mpv/` 下任何文件后**必须重跑 `./scripts/build_mpv.sh`**——`build/mpv` 是合并副本，单独对其增量 `ninja` 会静默沿用旧副本掩盖修改（完整重建才会暴露，实测教训）。
- **`build_mpv.sh` 输出经 grep 过滤**——编译失败可能看不到；确认构建真正完成要检查最后一行 "Done"。
- **别 `cd build` 后再用 `./build/...`**——相对路径会失效；留在仓库根或用绝对路径。
- **系统库升级后**（FFmpeg、Qt、TensorRT、libcdio 等——pacman/apt）旧二进制会链旧 soname、直接启动失败：跑 `./scripts/build-from-source.sh` 一条命令重建（或手动 `./scripts/build_mpv.sh` + `ninja -C build`）。`./scripts/check-deps.sh` 会比对构建时快照并提示是否需要重建。
- **RIFE 引擎**（冻结后端，仅源码构建）：用系统 `trtexec` 构建（`bash tests/fruc/build_rife_full_engine.sh`，动态 shape FP16；分发用 `--hardware-compat on` 跨架构）。需要 `third_party/rife/` 下的 RIFE ONNX 资产。
- **开发安装**：仓库内直接跑 `./scripts/install.sh` 即可（dev 模式，自动识别构建树，无需 tarball）。
- **分发构建**：`./scripts/build_release.sh [--variant=standard|full] [--fruc-backend=vfg|rife]`——release + `$ORIGIN` 相对 RPATH + 依赖收集 + 闭包自检 + tarball。`--fruc-backend=vfg`（默认，也是唯一对外发布的）不捆绑 TensorRT、也不含 `engines/`；`--fruc-backend=rife` 才会加上 TensorRT 11 + RIFE 引擎（`--engine=<已构建引擎>` 可复用引擎跳过 trtexec）——想给自己打一个 RIFE 包就用它。本项目只分发 `--variant=standard`；`--variant=full`（自带 Qt）供 Qt < 6.12 的用户自建。`build_mpv.sh`/client 构建接受 `MPV_BUILD_DIR`、`BUILDTYPE`、`DIST_RPATH` 环境变量覆盖（发布脚本使用）。

## 版本兼容性

| 组件 | 绑定关系 | 不匹配时 |
|------|----------|----------|
| **VFX SDK ↔ 驱动** | NVIDIA 索引最新 wheel 可能要求更新的驱动；旧驱动 + 新 VFX → VSR 加载失败 | 升级驱动，或锁定旧版 VFX（见下） |
| **RIFE 引擎 ↔ TensorRT**（冻结后端，仅源码构建） | 引擎文件内嵌构建时的 TRT 精确版本——版本不匹配时反序列化直接拒绝（双向均实测） | 用当前系统 TRT 重建引擎（`bash tests/fruc/build_rife_full_engine.sh`），或装匹配版本 TRT。VFX 自带的 TRT 10 与 RIFE 的 TRT 11 同进程共存（RTLD_LOCAL 隔离），无需处理。默认 VFG 后端不涉及 TensorRT，本行不适用 |
| **Qt** | standard 变体硬性要求 ≥ 6.12（用到的 QML/QuickControls 特性） | 升级系统 Qt，或用 `--variant=full` 自建 full 变体（自带 Qt6，无系统 Qt 要求）；不再有预编译 full 包 |
| **GPU** | VSR 需 RTX 20+；插帧默认的 VFG 后端需 **Ada/Blackwell**，冻结的 RIFE 源码构建后端需 Ampere+（FP16 Tensor Cores） | 旧 GPU：VSR 可用，插帧不可用（直通） |
| **驱动** | VFX 需 570+；Wayland 在现代驱动上无需内核参数（modeset 默认开启） | 升级驱动，或锁定旧版 VFX wheel |

**锁定 VFX SDK 版本**——install.sh 总是从 **NVIDIA 官方索引**（`pypi.nvidia.com`；pypi.org 上只有占位 sdist）拉取**最新** `nvidia-vfx` wheel。如果默认版本在你的环境不工作（如驱动太旧），不必被迫使用它：

```bash
# 1. 查看可用版本（wheel 在 NVIDIA 索引，pypi.org 没有）
pip index versions nvidia-vfx --extra-index-url https://pypi.nvidia.com

# 2. 下载指定版本 wheel（pip download 只取文件，不安装）
pip download nvidia-vfx==<版本> --no-deps --extra-index-url https://pypi.nvidia.com -d /tmp/vfx

# 3. 解压到应用库目录 + VFX 库 RPATH 期望的兄弟目录
unzip -o /tmp/vfx/nvidia_vfx-<版本>*.whl "nvvfx/libs/*" -d /tmp/vfx
cp /tmp/vfx/nvvfx/libs/*.so* ~/.local/lib/vsr-player/
# wheel ≥ 0.2.0.0 另有 nvidia_vfx.libs/（libcudart-<hash>.so.*）；VFX 库 RPATH 是
# $ORIGIN/../../nvidia_vfx.libs，必须落到应用库目录的兄弟位置：
unzip -o /tmp/vfx/nvidia_vfx-<版本>*.whl "nvidia_vfx.libs/*" -d /tmp/vfx
mkdir -p ~/.local/nvidia_vfx.libs
cp /tmp/vfx/nvidia_vfx.libs/*.so* ~/.local/nvidia_vfx.libs/ 2>/dev/null || true
```

注：0.2.0.0 之前的 wheel 没有 `nvidia_vfx.libs/`（unzip 会提示 filename not matched，可忽略）；0.2.0.0 起漏掉它会让 `libVideoFXLocal.so` 加载失败 → **VSR 静默直通**。

注意：`vsr_proc.c` 以**无版本名** dlopen（`libnppc.so`、`libcudnn.so`、`libnvidia-ngx-vsr.so` 等），而 wheel 只含带版本名文件（`.so.12`、`.so.9`……）。install.sh 的自动下载路径会补无版本软链；**手动放置时需自行补齐**，否则 VFX 加载链断裂（超分静默直通）：

```bash
cd ~/.local/lib/vsr-player/
for t in libnppc libnppial libnppicc libnppidei libnppig libnppif \
         libnppim libnppist libnppitc libcudnn libnvidia-ngx-vsr; do
  for s in "$t".so.*; do [ -e "$s" ] && ln -sf "$s" "$t.so" && break; done
done
```

install.sh 从不把任何版本强加给你的系统——一切都在 `~/.local/lib/vsr-player/` 内，替换该目录下的 VFX 文件即是官方支持的切换方式。

## 故障排查

### 多显卡（核显 + 独显、或两张 NVIDIA）：黑屏 / 超分与插帧不可用 / 无法硬解

本项目整条 AI 链是 **CUDA 产出 → Vulkan 零拷贝导入**，所以 **CUDA 设备与 Vulkan 设备必须是同一张卡**。下面两类症状都是"两张卡错位"：

- 打开超分/插帧后画面**全黑**（音频、控件正常）；
- 或日志出现 `AI 处理不可用：Vulkan 设备 … 没有 UUID 相同的 CUDA 设备` / `decoder: 解码切换禁用：此设备无法硬解（CUDA 不可用）`（这时是**明确降级**：不黑屏，但既没 AI 也没硬解）。

**先看启动日志选的是哪张卡**：

```
[vsr] GPU: <设备名> uuid=<前 4 字节>…
[vsr] AI 处理可用：<同一张卡> ↔ CUDA <同一张卡>        # 说明选对了
```

**让 Vulkan 与 CUDA 落在同一张卡**（两条都要指同一张，`uuid` 用 `nvidia-smi -L` 或 `vulkaninfo --summary` 的完整串）：

```bash
# ① 只让 Vulkan 看到 NVIDIA（笔记本核显+独显场景够用）
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
# ② 让 CUDA 的 0 号就是这张卡（两张 NVIDIA 时必需）
CUDA_VISIBLE_DEVICES=GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx \
vsr-player <视频>
```

⚠️ 两张 NVIDIA 时，Vulkan 侧没有可靠的"按设备"环境变量（`VK_LOADER_DEVICE_SELECT` 在很多 loader 上无效）；这时以日志里的 `GPU:` 行为准，把 `CUDA_VISIBLE_DEVICES` 钉到**同一张**即可。滤镜内部已按 Vulkan 设备 UUID 选 CUDA 设备（`vf_hwup` 的 `cuda-uuid` 选项），所以软解码路径也不会再选错卡；选不到时会打 WARN 并退回 0 号。

### 启动失败：`error while loading shared libraries: libXXX.so.NN`

**根因**：系统库升级后 **soname 变了**（实测例子：libcdio `.so.19 → .so.21`、libbluray `.so.3 → .so.4`），而已构建的二进制仍链着旧名字。这不是应用 bug，而是"二进制与系统库版本错配"——几乎所有 Linux 原生程序都会遇到，源码构建路径的整个设计就是为了消除它。

**诊断**：

```bash
./scripts/check-deps.sh
```

- `❌ … 缺失: libXXX.so.NN` → 该 soname 已从系统消失（致命，必须重建/换包）
- `⚠ libXXX 自构建后被替换` → 库文件被升级替换（建议重建）
- 两项都正常 → 另有原因；报 issue 时附 `ldd ~/.local/bin/vsr-player | grep "not found"` 与 `./scripts/check-deps.sh` 输出

**处理（按安装方式）**：

| 安装方式 | 处理 |
|---|---|
| **源码构建** | 重跑 `./scripts/build-from-source.sh`——一条命令重建出与新系统库匹配的二进制。**这是推荐源码构建的核心原因**：你能立刻自救，不必等新版本发布 |
| **standard 包**（自建的 full 变体另含 Qt） | 包里捆绑的部分（ffmpeg/CUDA/媒体栈——其中 TensorRT 与 RIFE 引擎仅存在于 `--fruc-backend=rife` 构建）不受影响；受影响的是包仍需**系统提供**的库（Qt、Vulkan、Wayland/X11、mpv 的可选依赖）。按序尝试：① 下载**更新版本**的 tarball（其捆绑集合已跟进）② 改用源码构建 ③ 临时救急见下 |

**临时救急（不推荐长期）**：给缺失的 soname 建兼容软链——**仅当新旧库 ABI 兼容时才安全**：

```bash
mkdir -p ~/.local/lib/compat
ln -sf /usr/lib/libcdio.so.21 ~/.local/lib/compat/libcdio.so.19   # 换成实际报错的库
LD_LIBRARY_PATH=~/.local/lib/compat vsr-player <视频或目录>
```

⚠️ soname 大版本跳跃通常意味着 ABI 有变化。本项目不使用的功能路径（CDDA、蓝光菜单）风险低，但**不要把它当成长期方案**：正解是重建（源码路径）或换用新包。

**不要做的**：在滚动发行版上单独降级某一个库（部分升级）——会引入更多库错配，比原问题更难收拾。

### 超分不生效（画面正常但没有超分效果）

VFX 运行时缺失或加载失败时，`vf_vsr` 会**静默直通**（视频照常播放、不报错），所以"看起来正常"不代表超分在工作。检查：

- 启动日志里是否有 `VSR: nvVFX libraries loaded`；没有就是没加载上
- `ls ~/.local/lib/vsr-player/libNVCVImage.so`，以及 `libnppc.so`、`libcudnn.so` 等**无版本名**软链是否齐全（`vsr_proc.c` 按无版本名 dlopen，缺软链则整条 VFX 链断裂）
- 重新获取：跑 `./scripts/install.sh`（从 NVIDIA 官方索引下载），或按 [docs/third-party-setup.md](docs/third-party-setup.md) 手动放置
- 驱动过旧也会导致 VFX 加载失败：**VFX 版本 ↔ 驱动**需匹配（见下方"锁定 VFX SDK 版本"）

### 插帧不生效

先确认这次构建用的哪个后端——启动日志里的滤镜状态行：VFG（默认）是 `vfg-status:`，RIFE 是 `fruc-status:`。默认的 **VFG** 后端需 **Ada/Blackwell**（见"版本兼容性"）；旧 GPU 上该滤镜不生效。

**仅 RIFE 后端**（`--fruc-backend=rife` 源码构建）：RIFE 引擎内嵌了**构建时的 TensorRT 精确版本**，TRT 升级后旧引擎会被拒绝反序列化（表现为插帧退化为直通）。用当前系统 TRT 重建：

```bash
bash tests/fruc/build_rife_full_engine.sh
```

### 首次运行像是卡住 10–15 秒（引擎内核缓存）

在**第一次**运行的 HOME 上（该 HOME 从未跑过 CUDA 计算），超分引擎加载时要现编译内核，画面会等 10–15 秒才出来。启动日志会写明：

```
first run on this HOME: engine kernel cache is empty (<path>) — the first super-resolution engine load compiles kernels and can take 10-15 s (one-time; later loads ~0.15 s)
```

- 每个 HOME 只付一次（`$HOME/.nv/ComputeCache`）；之后的引擎加载约 0.15 秒。
- 该缓存**与尺寸/倍率无关**：暖过一次后，换任何缩放倍率都一样快。
- **重启不会**让它失效；**驱动大版本升级**是否会失效**未验证**。
- 这只是提示、不是预热：首帧仍会等 10–15 秒。

## 使用

- 打开文件或目录（目录会把全部可播放文件加载进播放列表）
- 画质控制：底栏 `画质` 弹窗——缩放 off/auto/2×/3×/4×、超分质量、降噪、插帧，以及 **SDR→HDR10** 开关（旁边 `Advanced` 按钮进参数：目标峰值/亮度、饱和度、对比度、中灰、抗色带）
- 播放列表：`P` 键；UI 自动隐藏由底部悬停区域驱动（鼠标移出即隐藏）
- OSD：`Tab` 切换 mpv 渲染的信息浮层（源、输出——含**实际**的 HDR10/SDR 输出模式——HDR/内容状态、超分、插帧、解码器、倍速、显卡…）
- 启动：无参数启动只恢复上次的播放列表，**不载入媒体**（不解码、不启 VSR/插帧引擎、不占 GPU/显存）；按播放才载入当前条目，并自动从上次保存的进度继续。命令行给了文件、或显式 `-- --pause=yes` 时按 mpv 语义立即载入

### 命令行选项

| 选项 | 取值 | 默认 | 说明 |
|------|------|------|------|
| `--scale` | `off`、`auto`、`2`、`3`、`4` | `auto` | 超分倍率 |
| `--quality` | `low`、`medium`、`high`、`ultra` | `high` | 超分质量 |
| `--denoise` | `off`、`low`、`medium`、`high`、`ultra` | `off` | 降噪强度（scale=1 时生效） |
| `--fruc` | `off`、`2`、`3`、`4`（默认 VFG 后端）；`off`、`40`、`48`、`60`、`2`、`3`、`4`（RIFE 源码构建） | 持久化 | 插帧：整数倍率（VFG）或目标帧率 40/48/60（RIFE） |
| `--fruc-quality` | `low`、`medium`、`high` | `medium` | VFG 模型质量档（可热切换；RIFE 后端接受但忽略） |
| `--fruc-order` | `pre`、`post` | `post` | VFG 链序：`post` = 超分之后，`pre` = 超分之前（RIFE 固定） |
| `--fruc-max-fps` | `48`、`60`、`120` | `60` | 插帧输出帧率上限：源帧率 × 倍率超过它就直通（RIFE 后端接受但忽略） |
| `--no-hwaccel` | — | — | 关闭 NVDEC，使用软解 |
| `--lang` | 如 `en`、`zh_CN` | 系统 locale | 界面语言 |
| `--benchmark` | — | — | 无 UI 吞吐测量（`all=no` 日志） |
| `--vsync` | — | off | FIFO present（默认非阻塞；wayland 无撕裂问题） |
| `--no-rpc` | — | — | 关闭 JSON IPC 服务 |
| `--playlist` | `<文件>`、`-` | — | 播放列表文件（m3u/纯文本；空行与 `#` 注释跳过）；`-` = 从 stdin 读列表（同 mpv） |
| `--` | — | — | 其后的参数全部作为 mpv 选项透传（如 `-- --terminal=yes --ytdl-raw-options=cookies=c.txt`） |

位置参数即播放列表条目（同 mpv）：`vsr-player a.mkv b.mkv` 播放 `a.mkv` 并把
`b.mkv` 排进列表。应用自管的选项（`vo`、`vf`、`hwdec`、`msg-level`）不要写在
`--` 之后。

### 竖屏视频与插帧质量档

NVIDIA VFG 引擎对**竖屏/窄幅**输入会走一条明显更慢的内部路径（VFX SDK 1.3 的行为，
不是本项目的代码）：

| 输入（`--fruc-quality medium`） | 引擎单次插帧 |
|---|---|
| 1920×1080 横屏 | ≈2.5 ms |
| 3840×2160 横屏 | ≈3.4 ms |
| 1920×3328 竖屏 | ≈15–28 ms |

而且竖屏下三档的顺序会反转：同一 1920×3328 输入，LOW ≈4 ms、**MEDIUM ≈15–28 ms**、
**HIGH ≈5.5 ms**。所以竖屏内容建议在画质弹窗里把 **Interpolation quality 选 High**
（更快，画质也更好），或者选 Low；**避开 Medium**。换算成播放器里的实测：该竖屏源
30fps ×2 时 MEDIUM ≈15 ms/输出帧、HIGH ≈5.5 ms、LOW ≈4 ms。

数据可用 `tests/vfg/vfg_timing_probe.c` 复现（纯 NvVFX 探针，不经过播放器）。

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

带 `vf_hwup` + `vf_vsr` + `vf_truehdr` + `vf_vfg` 滤镜（RIFE 构建另含 `vf_rife`）的 patched mpv 也独立构建分发，可作普通 mpv 替代：

```bash
mpv-vsr --hwdec=auto --vf=hwup,vsr:scale=2,truehdr:enable=yes:luminance=650,vfg:fps=2:mode=medium video.mkv
```

滤镜链（从左到右）：`hwup`（SW→CUDA 上传，软解路径可用）→ `vsr`（超分）→ `truehdr`（SDR→HDR10，默认关）→ `vfg`（VFG 插帧）——默认链序，选取原则是每个环节都按源帧只计费一次（见[管线](#管线)）；要 `pre` 顺序就把 `vfg` 放在 `vsr` 前面。RIFE 构建里链序是 `hwup` → `rife`（插帧，源分辨率）→ `vsr` → `truehdr`。硬解（`--hwdec=nvdec`）时 `hwup` 为纯直通可省略。

滤镜选项：

| 选项 | 取值 | 说明 |
|------|------|------|
| `scale`（vsr） | `off`、`auto`、`2`、`3`、`4`、比例（如 `4/3`） | 超分倍率；`auto` 按视口选择 |
| `quality`（vsr） | `low`、`medium`、`high`、`ultra` | VSR 推理质量 |
| `denoise`（vsr） | `off`、`low`、`medium`、`high`、`ultra` | 降噪（scale=1 时单独生效） |
| `enable`（truehdr） | `yes`、`no` | SDR→HDR10 上变换（另有自守卫：HDR 帧直通） |
| `luminance`（truehdr） | 400…2000 | 目标 HDR 峰值 nit（默认 650；SDK 对区间外取值直接拒绝） |
| `saturation` / `contrast`（truehdr） | 0…200 | 默认 100 |
| `middle-gray`（truehdr） | 10…100 | 默认 50 |
| `debanding-off`（truehdr） | `0`、`1` | `1` 跳过抗色带（仅诊断；默认 `0` = 开） |
| `fps`（vfg） | `off`、`2`、`3`、`4` | 插帧倍率（VFG，默认后端） |
| `mode`（vfg） | `low`、`medium`、`high` | VFG 模型质量档（默认 `medium`） |
| `fps`（rife） | `off`、`auto`、整数 1..120 | 插帧目标帧率（仅 RIFE 构建）；`auto` = 2×源 |
| `scale`（rife） | `off`、`2`、`3`、`4` | benchmark 倍率（测设备上限；仅 RIFE 构建） |

除 RIFE 那两项外，其余都能在运行期经 `vf-command` 改，例如通过 JSON IPC socket 发
`{"command":["command","vf-command","truehdr","luminance","1000"]}`。

示例：`mpv-vsr --hwdec=nvdec --vf=vsr:scale=auto,quality=ultra,vfg:fps=2:mode=medium video.mkv`

`mpv-vsr-wrapper.py`（浏览器集成，ff2mpv 风格）：导出 Chrome cookie、yt-dlp 提取播放列表（YouTube/Bilibili/Niconico）、拉起 mpv-vsr。个人自用工具，不随发布包分发——有兴趣可自行研究。

## 开源协议

VSR Player 采用 GNU General Public License v2 或更高版本 (GPLv2+)。

完整协议文本见 [LICENSE](LICENSE)。

项目基于 [mpv](https://github.com/mpv-player/mpv)（GPLv2+）构建：编译带自定义
`vf_vsr`/`vf_vfg`/`vf_truehdr` 滤镜的 patched mpv，前端链接 libmpv——因此分发的二进制是 mpv 的
衍生作品，整个项目按 GPLv2+ 分发。Qt/QML 前端代码本身为独立开发。

运行时动态加载的 NVIDIA Video Effects SDK 库为 NVIDIA 专有软件，
不受本项目 GPL 许可证约束。

---

[English](README.md)
