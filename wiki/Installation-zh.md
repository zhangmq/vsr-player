# 安装

> [English](Installation) · 中文

三种方式，按推荐程度排序。

## 运行要求

| 组件 | 要求 | 适用范围 |
|-----------|-------------|------------|
| GPU | NVIDIA RTX 20 系列或更新，用于 VSR；插帧的默认 VFG 后端需要 **Ada/Blackwell**（冻结的 RIFE 源码构建后端需要支持 FP16 Tensor Core 的 Ampere 及以上） | 全部 |
| Driver | 570+（含 CUDA；Wayland 开箱即用 —— 现代驱动默认启用 DRM modeset） | 全部 |
| VFX SDK | NVIDIA VFX SDK **1.3**（`nvidia-vfx` 0.2.0.0 wheel）—— 提供 VSR、VFG **和** TrueHDR；`install.sh` 从 NVIDIA 官方索引（`pypi.nvidia.com`）下载并解包，绝不装进 Python 环境 | 全部 |
| Qt | 6.12+（Quick、QuickControls、Vulkan） | 源码构建 / standard 压缩包（没有预编译的 full 包 —— 请用 `--variant=full` 自行构建） |
| C++ 编译器 | GCC 13+（C++20） | 源码构建 |
| 构建工具 | meson、ninja、CUDA Toolkit（`/opt/cuda`）；TensorRT（`trtexec`）仅用于构建冻结的 RIFE 后端 | 源码构建 |

第三方 SDK（NvVFX 头文件/运行时、MDI 图标字体、mpv 源码）**不**随仓库打包 —— 准备 `third_party/` 的方法见
[docs/third-party-setup.md](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md)（源码构建需要这一步）。

## 方案 A（推荐）：源码构建

在自己的机器上构建：生成的二进制**天然匹配你的系统库**，系统升级后重跑同一条命令即可修好 —— 不会出现
「soname 过期 → 启动失败」那一类故障（本项目在 libcdio/libbluray 升级时真实踩过）。

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# 1) prepare third_party/ per docs/third-party-setup.md
#    (NvVFX runtime, MDI font, mpv source; RIFE ONNX only for -Dfruc_backend=rife)
./scripts/build-from-source.sh
```

一条命令全包：依赖自检 → 构建 libmpv + 客户端 → 安装到 `~/.local`（无需 sudo）→ 记录依赖快照。首次运行会从
**NVIDIA 官方索引**抓取 VFX 运行时（wheel 约 0.6 GB，解包后约 1.1 GB）；已经有时可加 `--no-vfx` 跳过，另有
`--no-install` / `--jobs N`。

**系统升级之后**（Qt/FFmpeg/媒体库的 soname 变化会让旧二进制失效）：

```bash
./scripts/check-deps.sh              # shows drift (build-time snapshot vs current system libs)
./scripts/build-from-source.sh       # rebuild with one command
```

## 方案 B：预编译包

适用于不想编译，或发行版缺少 FFmpeg 9.x 开发包的情况。

| | standard（唯一分发的预编译包） | full（仅源码构建 —— `--variant=full`） |
|---|---|---|
| 包内携带 | **薄包（约 7 MB）**：`vsr-player`、`mpv-vsr`、`libmpv.so.2`、两个 Wayland 辅助模块（`libvsr-color-feedback`、`libvsr-idle-wayland`）、两个打过补丁的 Qt 6.12.0 库（仅当系统 Qt 仍是 6.12.0 时使用 —— Arch/CachyOS 现在已是 6.12.1）、`deps.manifest`（系统必须提供什么的可机读清单）、图标字体、翻译、许可证、桌面资源、`COMPONENTS.md` | 同样的内容 **+ 所有在发行版升级中真实出过问题的东西**：FFmpeg ×7、CUDA 运行时、Qt6（库 / 平台插件 / QML 模块）、光盘媒体栈、libplacebo 及其着色器栈、libmujs、libsndio |
| 来自系统 | **其余全部**：FFmpeg 9.x（`libav*`，`NEEDED` 去版本化并在安装时校验）、CUDA 驱动/运行时、Vulkan loader、Wayland/X11 客户端库、音频栈（PipeWire/Pulse/ALSA/JACK）、媒体栈（libcdio/libdvd/libbluray）、libplacebo、libass、libarchive 等，以及 **Qt ≥ 6.12** | 只保留无法携带的 ABI：glibc/C++ 运行时、NVIDIA 驱动、Vulkan loader、DRM/GBM、X11/Wayland 客户端库、音频服务器库 |
| VFX 运行时 | **从不打包**（NVIDIA 的 SLA 禁止再分发）—— `install.sh` 从 NVIDIA 索引下载，或你自行放到 `~/.local/lib/vsr-player/` | 同上 |
| 适用 | 使用 FFmpeg 9.x 与 Qt ≥ 6.12 的滚动发行版（Arch/CachyOS/Fedora…） | Qt < 6.12 的发行版，或希望 Qt 自包含时 |

`standard` 按约定就是薄包：系统库留在系统里，包内提供 `deps.manifest` —— 每个必需的 soname 一行，并注明谁需要它。
`install.sh` 会校验清单，**缺东西时拒绝安装**，打印缺失的 soname 及其所属者，而不是甩给你一行光秃秃的加载器错误。
`full` 则反过来，携带所有在发行版升级中真实出过问题的库；只有系统 ABI（glibc、NVIDIA 驱动、Vulkan loader、
DRM/GBM、显示/音频客户端库）留在外面。FFmpeg 是唯一把 `NEEDED` 条目**去版本化**的依赖（`libavcodec.so` 不带
soname 后缀），从而不锁定发行版版本；安装器会记录并校验主版本号：

- `lib/vsr-player/VSR-FFMPEG-MAJOR` 记录构建该包所用的 FFmpeg 主版本；安装器会与目标系统比对，不一致时告警。
- 缺失的 soname 会被明确报出（并带兼容软链兜底：用 `ldd -r` 校验，缺符号则回滚）。
- 若 `ldd` 把 `libmpv.so.2` 解析到系统 mpv 而非包内那份，属硬错误 —— 这种组合「能跑」，但会静默丢掉所有 AI 处理环节。
- `COMPONENTS.md`（打包时由产物生成）列出包内携带的内容、对系统的要求，以及每个库对 mpv 的要求。

实践中的含义：`standard` 压缩包不会因为 `pacman -Syu` 升级了某个光盘/placebo/sndio 库就挂掉，但它仍然要求
发行版相对较新（FFmpeg 与 Qt）。如果你的 FFmpeg/Qt 较旧 —— 或者希望完全不受系统媒体库影响 —— 请源码构建
（同样的权衡，只是本地编译）或构建 full 变体。

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                         # installs to ~/.local, no sudo
```

用 `pkg-config --modversion Qt6Quick` 检查 Qt：≥ 6.12 → 用 standard；否则自行构建 full 变体。

**在没有 NVIDIA GPU 的机器上构建** —— `bash scripts/build-from-source.sh --no-rtx`（mpv 构建则用
`-Dno_rtx=true`）会完全去掉 CUDA/VFX 部分，因此二进制能在没有 N 卡的机器上启动。AI 功能从构造上就不存在；
这用于测试播放器外壳与打包检查。下限是 6.12，因为包是用 **Qt 6.12.0** 构建的，而 Qt 6 的二进制兼容性是
**单向**的（用 6.x 构建的包能在 6.x 及更新的 6.y 上运行，不能在更旧的版本上运行）。

**可选的 `-rife` 包已停止提供**（自 0.4.0 起）：RIFE 后端已冻结，不再以预编译包的形式分发。它仍作为
源码构建选项保留 —— `./scripts/build-from-source.sh --fruc-backend=rife`，或用
`./scripts/build_release.sh --fruc-backend=rife` 自行打包 —— 这对 Ampere 及更旧的 GPU 有意义，因为默认的
VFG 后端需要 Ada/Blackwell。

安装器还会配置**桌面集成**：应用菜单/启动器中会出现一个 **VSR Player** 条目（可固定到任务栏，可设为视频/音频
文件的默认处理程序）。包内以两种形式携带应用图标 —— 可缩放 SVG 与 512 px PNG —— 安装到
`~/.local/share/icons/hicolor/{scalable,512x512}/apps/`，启动器、任务栏和文件管理器显示的就是它们。
`data/vsr-player.svg` 是唯一来源：`scripts/render-icon.sh` 由它重新生成 PNG，**About** 对话框中嵌入的也正是
这张 PNG，两者因此不会分叉。

- **VFX 运行时**（wheel 约 0.6 GB，解包约 1.1 GB）**不**打包（NVIDIA 的 SLA 禁止再分发）：`install.sh` 从
  **NVIDIA 官方索引 `pypi.nvidia.com`** 下载并解包（纯 curl，不用 pip，不改动系统），你也可以手动放到
  `~/.local/lib/vsr-player/`。
- 打包采用「显式核心集 + 自动发现的媒体栈 + **打包时闭合检查**」：任何既未打包、也不在系统白名单里的依赖都会
  让构建失败，因此坏掉的压缩包不可能静默流出。
- 两个变体都还依赖发行版的常规组件：Vulkan loader、Wayland/X11 客户端库，以及 mpv/FFmpeg 的可选依赖
  （libass、libplacebo、libmujs、liblcms2、libwebp、libopenjp2、1394 采集库等）—— 全部由闭合检查验证。

## 方案 C：手动构建（开发/调试）

1. **准备 `third_party/`** —— NvVFX SDK 头文件/运行时、MDI 图标字体、mpv 源码、CUDA 12 归档；RIFE ONNX
   资源仅 `-Dfruc_backend=rife` 需要：按
   [docs/third-party-setup.md](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md) 操作。
2. **构建并运行：**

```bash
./scripts/build_mpv.sh          # merge src/mpv overlay → third_party/mpv, build libmpv + filters
ninja -C build                  # build the Qt client
./build/src/client/vsr-player <video-or-directory>
```

注意事项 / 坑：

- **mpv 补丁机制**：`third_party/mpv` 是未改动的基线；`src/mpv/` 是 overlay（镜像 mpv 目录树，只含改动过的
  文件）。改动 `src/mpv/` 下任何内容后**必须**重跑 `./scripts/build_mpv.sh` —— `build/mpv` 是*合并后的
  副本*，只跑 `ninja` 会静默沿用旧副本。
- **`build_mpv.sh` 的输出经过 grep 过滤** —— 编译失败可能被藏起来。请确认出现了最后的 "Done" 行。
- **不要 `cd build` 之后再用 `./build/...`** —— 相对路径会失效。请留在仓库根目录，或使用绝对路径。
- **系统库升级之后**（FFmpeg、Qt、TensorRT、libcdio 等 —— pacman/apt）旧二进制会链接旧 soname 而启动失败：
  运行 `./scripts/build-from-source.sh`，或手动 `./scripts/build_mpv.sh` + `ninja -C build`。
  `./scripts/check-deps.sh` 会与构建时快照比对，告诉你是否需要重建。
- **RIFE 引擎**（冻结后端，仅源码构建）：用系统 `trtexec` 通过 `bash tests/fruc/build_rife_full_engine.sh`
  构建（动态 shape FP16，`--hardware-compat on` 以跨架构）。需要 `third_party/rife/` 下的 RIFE ONNX 资源。
- **开发安装**：`./scripts/install.sh` 也能直接从仓库检出运行（dev 模式 —— 自动拾取构建树，无需压缩包）。
  当存在过期的 `build/client-dist` 时，加 `--dev` 强制使用开发树。
- **可分发构建**：`./scripts/build_release.sh [--variant=standard|full] [--fruc-backend=vfg|rife]` ——
  release 构建 + `$ORIGIN` 相对 RPATH + 依赖收集 + 闭合检查 + 压缩包。只分发 `--variant=standard`；
  `--variant=full`（打包 Qt）供 Qt < 6.12 的用户自行构建。`build_mpv.sh`/客户端构建接受 `MPV_BUILD_DIR`、
  `BUILDTYPE`、`DIST_RPATH` 环境变量覆盖（发布脚本会用到）。

## 版本兼容性

| 组件 | 约束 | 不匹配时 |
|-----------|---------|---------------|
| **VFX SDK ↔ 驱动** | NVIDIA 索引上最新的 wheel 可能要求更新的驱动；旧驱动 + 新 VFX → VSR 加载失败 | 升级驱动，或锁定较旧的 VFX 版本（见下文） |
| **RIFE 引擎 ↔ TensorRT**（冻结后端，仅源码构建） | 引擎文件内嵌构建它的确切 TRT 版本 —— 版本不匹配会导致反序列化失败（双向均已验证） | 用系统 TRT 重建引擎（`bash tests/fruc/build_rife_full_engine.sh`），或安装匹配的 TRT。VFX SDK 自带的 TRT 10 库与 RIFE 的 TRT 11 可在同一进程内共存（RTLD_LOCAL 隔离）。不适用于默认的 VFG 后端 |
| **Qt** | 源码构建与 standard 压缩包硬性要求 ≥ 6.12 | 升级系统 Qt，或自行构建 full 变体（`--variant=full`） |
| **GPU** | VSR 需要 RTX 20+；默认的 VFG 插帧后端需要 **Ada/Blackwell**，冻结的 RIFE 源码构建后端需要 Ampere+ | 更旧的 GPU：VSR 可用，插帧不可用（直通） |
| **驱动** | VFX 需要 570+ | 升级，或锁定较旧的 VFX wheel |

### 锁定 VFX SDK 版本

`install.sh` 从不把版本强加到你的系统上 —— 所有文件都在 `~/.local/lib/vsr-player/`，替换那里的 VFX 文件
就是受支持的切换方式。

```bash
# 1) list available versions (wheels live on NVIDIA's index, not pypi.org)
curl -s https://pypi.nvidia.com/nvidia-vfx/ | grep -o 'nvidia_vfx-[0-9.]*-py3-none-manylinux[^"#]*' | sort -u

# 2) download a specific version's wheel (pip download only fetches; no install)
pip download --no-deps --dest /tmp/vfx nvidia-vfx==<version> --index-url https://pypi.nvidia.com

# 3) extract into the app's lib dir + the sibling dir the VFX libs' RPATH expects
cd /tmp/vfx && unzip -o nvidia_vfx-*.whl
cp nvvfx/libs/* ~/.local/lib/vsr-player/
mkdir -p ~/.local/nvidia_vfx.libs && cp nvidia_vfx.libs/* ~/.local/nvidia_vfx.libs/
```

`wheel ≥ 0.2.0.0` 还会带上 `nvidia_vfx.libs/`（`libcudart-<hash>.so.*`）；VFX 库带
`RPATH $ORIGIN/../../nvidia_vfx.libs`，因此该目录必须落在应用 lib 目录旁边（如上），否则 VFX 链加载失败。
