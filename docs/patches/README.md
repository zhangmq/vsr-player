# patches/

放**上游已修复、但我们依赖的发行版还没带上**的补丁，一个补丁一个文件，文件名写清组件与上游编号。

## qt6-declarative-QTBUG-151831.patch

- **问题**：Qt 6.12.0 的两个 QSG 渲染循环在 `FrameOpSwapChainOutOfDate` 之后**不重建
  swapchain**（只要窗口尺寸没变就继续复用旧的）。NVIDIA Vulkan 驱动在 Wayland 合成器
  更新 surface 的 dmabuf feedback 时会持续返回 `VK_ERROR_OUT_OF_DATE_KHR`（典型触发场景：
  **切全屏**），于是之后每一帧都以同样方式失败、窗口永久不再出帧，只有 surface 重建
  （resize / hide-show / 退出全屏）才能救回来。
- **上游**：QTBUG-151831，修复提交 `89499484`（`Pick-to: 6.8`），6.12 分支 cherry-pick
  `2921382a52`。Qt 6.12.1 起自带此修复，届时本补丁可删。
- **本机症状与证据**：见 `docs/fullscreen-render-stall-investigation.md` §4.1
  （Vulkan WSI 层抓到的返回码：全屏后 `acquire` 返回 `-1000001004` 且**再没有
  `vkCreateSwapchainKHR`**）。
- **注意**：只替换系统 `libQt6Quick.so.6.12.0` 时，必须把 Arch 自己打在 qt6-declarative
  上的 cherry-pick 一起编进去（`prepare()` 里的 3 个提交），否则会丢掉 KDE/Plasma 的修复。
