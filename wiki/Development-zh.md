# 开发

> [English](Development) · 中文

## 仓库结构

```
src/mpv/            patch overlay for mpv (only modified files; mirrors the mpv tree)
src/client/         Qt 6 + QML frontend (MpvController, PlayerViewModel, StereoController, RPC)
third_party/mpv     pristine mpv source (not modified by hand)
scripts/            build / install / release / translation scripts
tests/              probes and stress tests (VSR/VFG timing, 3D conversion, fullscreen stress)
docs/               engineering notes and measurements
wiki/               source for the GitHub wiki (see wiki/README.md)
```

## mpv 补丁机制

`third_party/mpv` 是未改动的基线；`src/mpv/` 是 overlay。`scripts/build_mpv.sh` 把它们合并到
`build/mpv`（发布时是 `build/mpv-dist`）并构建 libmpv。

**改动 `src/mpv/` 下任何内容后，必须重跑 `./scripts/build_mpv.sh`。** `build/mpv` 是*合并后的
副本*：只跑 `ninja -C build` 会静默沿用旧副本，表现就像「滤镜不在链上」。客户端同理：
`ninja -C build` 不会重建 libmpv。

自定义滤镜位于 `src/mpv/video/filter/`：`vf_hwup`、`vf_vsr`、`vf_truehdr`、`vf_vfg`、`vf_rot`、
`vf_eye`（外加已冻结的 `vf_rife`）。

## 构建命令

| 命令 | 用途 |
|---|---|
| `./scripts/build_mpv.sh` | 合并 overlay + 构建 libmpv 与滤镜 |
| `ninja -C build` | 构建 Qt 客户端 |
| `./scripts/build-from-source.sh` | 端到端：依赖检查 → 构建 → 安装 → 依赖快照 |
| `./scripts/build_release.sh [--variant=standard\|full] [--fruc-backend=vfg\|rife]` | 发布构建 + dist RPATH + 依赖收集 + 闭合检查 + 压缩包 |
| `./scripts/install.sh [--dev] [--no-vfx] [--no-rtx]` | 安装到 `~/.local`（dev 模式读取构建树；`--dev` 强制使用；`--no-vfx/--no-rtx` 可避开 900 MB 的 VFX 运行时） |
| `./scripts/check-deps.sh` | 将构建时的依赖快照与当前系统比对 |
| `./scripts/update-translations.sh` / `--check` | 从源码刷新 `.ts` / 校验完整性 |

`--no-rtx`（mpv 构建则用 `-Dno_rtx=true`）还会关掉滤镜链的 CUDA/VFX 一半，没有 NVIDIA GPU 的
机器就是靠它做冒烟测试的。

值得知道的坑：

- `build_mpv.sh` 会过滤输出 —— 编译失败可能被藏起来；请找最后的 "Done" 行。
- `install.sh` 以原子方式替换文件（临时文件 + 重命名），因此播放器运行时执行安装不会让正在运行的实例
  崩溃。
- 开发构建带的是构建树 RPATH；若要让装好的二进制加载随包携带的库，请走发布路径（`build_release.sh`
  → `install.sh`），而不是开发树。

## 测试与探针

| 路径 | 作用 |
|---|---|
| `tests/vfg/vfg_timing_probe.c` | 纯 NvVFX 的 VFG 计时，不涉及播放器（横屏与竖屏路径） |
| `tests/fs_stress.sh` | 全屏切换压力测试（设备丢失回归） |
| `tests/hotkeys/test_hotkeys.py` | 键位表、弹出面板交接、菜单状态机、单眼分支 —— 通过 JSON IPC 驱动真实播放器并注入按键 |
| `tests/hotkeys/test_stereo_keys.py` | 用第二块屏幕充当眼镜的真实 3D 会话：按键路由、帮助叠加层、两个窗口、退出/恢复 |
| `tests/hotkeys/test_thumbnails.py` | 悬停缩略图：尺寸与 DPR、网络流、首次悬停回放、重试冷却、菜单开关 |
| `tests/fruc/build_rife_full_engine.sh` | 用系统 `trtexec` 构建冻结的 RIFE TensorRT 引擎 |
| `tests/ff2mpv/*.c` | 接入浏览器集成 wrapper 时用的 libmpv 选项/终端探针 |
| `tools/mvc2sbs` | 离线 MVC → 全宽 SBS 转换 |

所有快捷键只有一个事实源：`src/client/Keymap.cpp`（一张表），由 `KeyRouter` 执行。QML 从不为应用动作
声明 `Shortcut` 或比较 `Qt.Key_*` —— 右键菜单与 3D 选择器通过同一张表解析物理按键
（`keymap::findMenu` → `keys.menuAction`），因此每个绑定只存在一处。`check_keys.py`
（`scripts/precommit.sh` 也会运行它）负责强制执行这一点，并拒绝重复绑定、未实现的动作以及重新漏回
QML 的快捷键。

`tests/hotkeys` 下的脚本在运行期间会占用窗口、键盘和音频，所以请在没有其他播放时运行。

很多行为不用看屏幕就能通过 JSON IPC 验证（`get-playlist`、`get-video-size`、`get-stereo`、
`get-source-format`、`set-eye`、`set-rotate`、`grab-window`、`vf-command`）。这是检查几何、
滤镜状态与播放列表内容的首选方式。

## 调试技巧

- 日志输出到 stderr；用 `-- --msg-level all=v` 提高详细程度（帧级：`all=dbg`）。`--benchmark`
  无界面且安静（`all=no`）。
- OSD（`Tab`）报告实际输出格式、VSR 倍率与耗时、插帧状态以及所选解码器 —— 往往足以区分「直通」与
  「正常工作」。
- GUI 线程注意事项：媒体播放期间绝不要在 GUI 线程上调用同步的 mpv API（绘制线程同时负责 DR 图像
  分配）；请使用 `*Async` 变体，或通过观察器做反射。工程记录里有详细说明。

## 设计记录（`docs/`）

| 文档 | 主题 |
|---|---|
| `docs/hdr-pipeline-investigation.md` | 10-bit 链、HDR10 输出、色彩管理实测 |
| `docs/seekbar-thumbnail-research.md` | 悬停缩略图设计：几何、DPR、网络流抽帧 |
| `docs/vfx-sdk-1.3-investigation.md` | VFX SDK 1.3 的能力与 API 发现 |
| `docs/vfx-truehdr-investigation.md` | SDR→HDR10 上变换的参数与行为 |
| `docs/mvc-3d-open-source.md` | MVC 解码现状 / 为什么不在播放器里做 |
| `docs/ff2mpv-support-research.md` | 浏览器集成记录 |
| `docs/trt-engine-investigation.md` | 冻结 RIFE 后端的 TensorRT 引擎兼容性 |
| `docs/fruc-distribution-design.md` | 插帧后端与打包 |
| `docs/mpv-reference-index.md` | 打补丁时使用的 mpv 源码参考索引 |

## 参与贡献

最有用的 bug 报告包含：GPU 型号、驱动版本、`vsr-player --benchmark` 输出或 `all=v` 的日志，
以及确切的片源信息（分辨率、帧率、HDR/3D 排布）。本项目刻意对 agent 友好：代码、mpv overlay 与
设计记录都写得让 AI 编码 agent 能顺着管线走并快速定位问题。
