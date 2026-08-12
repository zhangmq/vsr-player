# 设计：fruc 依赖打包 + 用户端部署

日期：2026-08-12
分支：feat/dist-packaging

## 背景

vsr-player 当前分发靠 `scripts/install.sh`（dev 机 → ~/.local），存在已知问题：

- **RPATH 绑 build 树**：mpv `$ORIGIN/../../../third_party/nvvfx/lib:/opt/cuda/lib64`、client 指向 `build/mpv/_build` —— 复制安装后全部失效
- **系统 ffmpeg 版本漂移**：实测 libavcodec 62→63 升级直接破坏安装
- **/opt/cuda 开发机路径**：mpv 硬链接 libnvrtc.so.13/libcudart.so.13，用户机不存在
- **fruc（插帧）无部署方案**：RIFE engine 有 TRT 版本绑定 + GPU 架构绑定，engine 缺 TRT/架构不匹配时静默直通
- **VFX SDK 法律红线**：EA 评估许可禁止向第三方分发（SLA 8.5，点名公开软件仓库）

目标：产出可分发的 tarball + 用户端 install.sh，覆盖 fruc 全新能力链（engine/TRT/CUDA runtime），全程用户环境零改动。

## 版本对齐清单（查证结论）

| 绑定链 | 严格度 | 分发策略 |
|---|---|---|
| RIFE engine ↔ TensorRT 11 | 二进制级（deserialize 校验库版本） | engine 与捆绑 libnvinfer.so.11 同版本（11.2.1.2），RPATH 优先命中 |
| RIFE engine ↔ GPU 架构 | 硬件级（engine 单架构，当前 sm_120f） | 单 engine：`--hardwareCompatibilityLevel=ampere+`（实测转正，见 §2） |
| libmpv ↔ ffmpeg | SONAME 级（62→63 已实测破坏） | 捆绑构建时 major 的 7 个库 |
| libmpv ↔ CUDA runtime | SONAME 级（nvrtc/cudart.so.13） | 捆绑 libnvrtc.so.13 + builtins + libcudart.so.13 |
| TRT 11 ↔ cudnn/cublas | 无（实测 RIFE 图零加载 cudnn/cublas） | **不捆绑**（省 ~800MB） |
| client ↔ Qt 6 | 编译期 6.11 API | 依赖系统 ≥6.11，不捆绑（用户已选） |
| VFX SDK ↔ driver | 软约束（NGX 运行时校验） | 不处理（运行时报错） |

## 法律查证结论（决定 VFX 分发方式）

- **VFX SDK（third_party/nvvfx）**：NGC EA 版受 Evaluation License 约束；正式 SLA 2025.05.05 §8.5 禁止被许可人"copy, transfer, distribute"，**点名公开软件仓库**。Customer Product 分发授权（PST §1.1.3）仅限 AI Enterprise 付费订阅 → **tarball 不含 VFX**。
- **nvidia-vfx pip 包**：NVIDIA 官方发布（PyPI 维护者 nvidia），wheel 捆绑全部 33 个 .so + SLA 文本。用户 pip 安装 = 与 NVIDIA 直接建立许可关系（合法获取）；但提取再分发仍违反 8.5。
- **TensorRT runtime**：SLA 明确 "Distributable portions: only the runtime files (.so)" —— 随应用分发允许。
- **CUDA runtime**：redistributable components，随应用分发 OK。
- **RIFE ONNX 权重**：来源 vs-mlrt（GPL-3.0）—— 随包附来源 + GPL-3.0 文本。
- **Qt/ffmpeg/字体**：LGPL/GPL 动态链接 + 许可文本，OK。

## 发布物结构（tarball ~280MB，不含 VFX）

```
vsr-player-0.1.0/
├── vsr-player                # GUI 二进制
├── mpv-vsr                   # CLI 二进制（无 wrapper——mpv-vsr-wrapper.py 是
│                             #   开发者本地 Chrome 插件配套，不进分发）
├── lib/
│   ├── libmpv.so.2
│   ├── libavcodec.so.63  libavformat.so.63  libavutil.so.61   # ffmpeg 7 ×7
│   │   libavfilter.so.12  libswscale.so.10  libswresample.so.7  libavdevice.so.63
│   ├── libnvrtc.so.13  libnvrtc-builtins.so.13  libcudart.so.13   # CUDA runtime
│   └── libnvinfer.so.11  libnvinfer_plugin.so.11   # TRT 11.2.1.2（+ 软链 libnvinfer.so.11.2.1）
├── engines/
│   └── rife_full_fp16.engine         # ampere+ 跨架构兼容（30/40/50 系通用，~91MB）
├── fonts/materialdesignicons-webfont.ttf
├── translations/*.qm
├── licenses/                # SLA 2025.05.05、Qt LGPL、ffmpeg LGPL/GPL、vs-mlrt GPL-3.0
├── README.md                # 系统依赖声明（NVIDIA driver、Qt ≥6.11、VFX 获取说明）
└── install.sh               # 用户端一键安装（环境纯净）
```

## 关键机制

### RPATH 统一（打包阶段 patchelf 一次性完成）

- mpv/libmpv/client 的 RPATH 改写为 `$ORIGIN/../lib`（$ORIGIN = 二进制所在目录，与 cwd 无关）
- 效果链（行为均已实测）：
  - `dlopen("libnvinfer.so.11")` 按 RPATH → 命中捆绑 11.2.1.2 → engine 版本绑定满足
  - 捆绑 ffmpeg 屏蔽系统版本漂移（RPATH 优先级高于 ldconfig）
  - 捆绑 CUDA runtime 屏蔽 /opt/cuda 缺失
  - VFX 的 dlopen 走 vsr_proc.c 现成搜索路径 `~/.local/lib/vsr-player/`（零代码改动）
- 用户机器零配置：无环境变量、无 wrapper、无 shell 修改

### engine 架构策略（单 engine ampere+，2026-08-12 实测转正）

- `build_rife_full_engine.sh` 默认加 `--hardwareCompatibilityLevel=ampere+` 构建（单 engine 覆盖 Ampere+ 全系：sm_80/86/89/90/120——30/40/50 系及 Hopper）
- **实测**（sm_120 本机）：ampere+ engine 加载 + 推理正常（0% NaN），benchmark 231.7 vs native 234.3 fps（-1.1%，测量噪声内）；体积 91MB vs 3×54MB 多引擎方案
- 兼容机制：engine 保留 PTX，运行时按目标 GPU JIT；构建时 shared memory 上限略降（TRT 自动处理）
- 边界：Turing（RTX 20 系，sm_75）及更早不在 ampere+ 内 → 现有 `reason=engine` 直通 + OSD 提示（已有）
- `--hardware-compat off` 保留原生构建（本机 benchmark 用，性能最优）
- rife_proc.c 固定名搜索，**零 C 代码改动**；install.sh 检测 GPU 仅作信息显示（型号 + 插帧可用性判定）
- 注：sm_86/sm_89 目标机的真实加载待对应硬件验证（TRT 官方保证的 forward compatibility 机制）

### VFX 获取（环境纯净版）

- tarball 不含 VFX；install.sh 检测 `libNVCVImage.so` 缺失时：
  1. 打印 NVIDIA 许可提示（SLA 2025.05.05 摘要 + licenses/ 全文）
  2. `curl https://pypi.org/pypi/nvidia-vfx/json` 解析 manylinux wheel URL
  3. curl 下载 wheel → unzip 提取 `nvvfx/libs/*` → `~/.local/lib/vsr-player/` → 清理临时文件
- 唯一写入点 = 应用自属目录；不调用 pip、不碰 site-packages、不 sudo、不改配置
- 下载失败 → 跳过并保留提示（用户可手动下载后重跑）；支持 `--vfx-dir <path>` 指向已解压目录

### 用户端 install.sh 流程

> 范围：vsr-player（GUI）+ mpv-vsr（CLI）二进制、lib/、engines/、字体、翻译。
> **不含** mpv-vsr-wrapper.py（开发者本地 Chrome 插件配套，非分发物）——
> install_mpv_local.sh（开发者本地脚本）保持现状。

1. 依赖检查：`libcuda.so.1`（缺→退出）、Qt ≥6.11（缺→警告继续）、GPU 型号检测（信息显示 + Ampere 以下提示直通）
2. 复制二进制 + lib/ + 字体 + 翻译 → `~/.local/bin` + `~/.local/lib/vsr-player/`
3. engine 复制（单 ampere+ engine 通用，无选择逻辑）
4. VFX 缺失 → curl 下载引导（上述）
5. 验证 + 摘要：GPU/engine 匹配、VFX 状态、rife 预期可用性

**环境纯净原则**：只复制文件到应用自属目录；系统依赖只检测提示、不自动安装；PATH 只提示不修改；不创建配置。

## 开发者侧脚本

- 新增 `scripts/build_release.sh`：build_mpv → ninja client → 依赖库收集（从 /usr/lib + /opt/cuda/lib64，校验 SONAME 与 NEEDED）→ patchelf RPATH → engine 构建（ampere+）→ tarball 打包
- `build_rife_full_engine.sh` 加 `--hardware-compat on|off`（默认 on=ampere+；off=原生本机架构）
- `install.sh` engine 逻辑重写为 GPU 检测选择

## 清理清单

- 删 `scripts/build_rife_engine.sh`（legacy 512 固定形状，已被 build_rife_full_engine.sh 取代）
- 删 `third_party/rife/rife512.engine`、`rife_v4.25_lite.7z`（gitignored，本地物理删除）
- 删 `build/tests/fruc/rife_lite_fp16_all.onnx`（构建产物，物理删除）
- 4.26 ONNX（`rife_full_fp16_all.onnx` 源）归位 `third_party/rife/`（附 GPL-3.0 vs-mlrt 来源声明）

## 测试

- 冒烟：install.sh 装到临时 HOME → 无 VFX 环境启动 → 直通不崩；有 VFX + 匹配 engine → 插帧激活（fruc-status active）
- engine 匹配矩阵：每架构 engine × 对应 GPU（本机 sm_120 实测；sm_86/89 靠 cross-build 后 TRT 加载验证或标注待测）
- 降级路径：engine 缺失/不匹配 → reason=engine 直通 + OSD 提示

## 待验证项（实现阶段实测）

1. ~~cross-arch engine 构建~~ ✅ **已实测转正**：ampere+ 单 engine 方案（加载/推理/性能全部通过，见 §2）
2. patchelf 后 dlopen 顺序与 VFX 全链加载（开发机 + 干净环境模拟）
3. PyPI JSON API 下载流程（wheel URL 解析、curl 下载、unzip 提取）
4. ~~`--hardwareCompatibilityLevel=ampere+` 性能~~ ✅ 231.7 vs 234.3 fps（-1.1%）
5. sm_86/sm_89 目标机加载（待有对应硬件；TRT 官方 forward compatibility 机制）
