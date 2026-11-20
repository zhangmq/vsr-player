# 关于本包内 `libQt6Quick` 的修改声明

本发布的 `lib/vsr-player/libQt6Quick.so.6`（full 变体为 `lib/libQt6Quick.so.6.12.0`）
是基于 **Qt 6.12.0** 源码构建的**修改版**，附带以下两个改动：

1. **上游补丁** `2921382a52`（QTBUG-151831，*QSG render loops: Recreate the swapchain
   when it is out of date*，改动 `libQt6Quick`），以及
   `c68e1fc1`（QTBUG-150279，*rhi: vulkan: Notify endFrame when beginFrame fails*，
   改动 `libQt6Gui`）。原始问题：Qt 6.12.0 在 Vulkan `FrameOpSwapChainOutOfDate`
   之后不重建 swapchain（尺寸未变即复用），而 NVIDIA 驱动在 Wayland 合成器更新 surface
   的 dmabuf feedback 时会持续返回 `VK_ERROR_OUT_OF_DATE_KHR`（切全屏为典型触发点），
   导致窗口永久停止出帧；同一条件下 `QRhiVulkan::beginFrame` 早退不通知 Wayland
   `endFrame`，surface 读锁泄漏，**关窗时 `QWaylandWindow::reset()` 自锁死**（进程/终端
   永不退出）。补丁全文见 `docs/patches/` 下两个 `.patch`。
2. **发行版补丁**：Arch Linux 的 qt6-declarative 在 6.12.0 上另外 cherry-pick 的 3 个提交
   （`8c23f65e`、`7214a320`、`88a046d8`，含 Plasma 退出挂起修复），保证与发行版行为一致。

## 许可

本包内的 Qt 库按 **LGPL-3.0-only** 使用（Qt 同时提供 GPL-3.0 与商业许可）；LGPLv3 以
GPLv3 为基础，因此本包同时附带 `licenses/LGPL-3.0-only.txt` 与
`licenses/GPL-3.0-only.txt`。完整对应源码 = Qt 6.12.0 官方源码 + 上述 4 个补丁，
补丁随仓库公开（`docs/patches/`、<https://github.com/qt/qtdeclarative>），构建脚本见
`build/qt-fix/README.md`；如需我们以其他形式提供对应源码，可来信索取。本库为动态链接、未做静态链接或私有改动封闭，
用户可随时用自己构建的同版本 `libQt6Quick.so.6.x.y` 替换本包内的这一份
（播放器 `RUNPATH` 为 `$ORIGIN/../lib/vsr-player:$ORIGIN/../lib`，替换即生效）。

## 获取对应源码

- 上游源码：`qtdeclarative-everywhere-src-6.12.0.tar.xz`（<https://download.qt.io/>）
- 本包所用改动：`docs/patches/qt6-declarative-QTBUG-151831.patch` + 上面 3 个提交
  （均可从 <https://github.com/qt/qtdeclarative> 以 `.patch` 形式下载）
- 完整重建步骤：仓库内 `build/qt-fix/README.md`（`cmake -G Ninja && ninja Quick`）

## 未来版本

系统 Qt 升到 **6.12.1 及以上**时该修复已包含在上游发行版中，安装程序会自动跳过本包内的
这一份（见 `scripts/install.sh` 的版本闸门），届时本声明与文件可一并移除。
