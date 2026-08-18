---
name: vfx-trt-symlink-rename
description: third_party/nvvfx/lib 无版本 TRT 软链改名记录（TRT 11 与 VFX TRT 10 冲突的修复）

# third_party/nvvfx/lib 未跟踪变更（2026-08-07）

**变更**：`third_party/nvvfx/lib/` 下三个**无版本软链改名**（保留文件，非删除）：

```
libnvinfer.so          → libnvinfer.so.unversioned
libnvinfer_plugin.so   → libnvinfer_plugin.so.unversioned
libnvonnxparser.so     → libnvonnxparser.so.unversioned
```

**原因**：VFX SDK 捆绑 TRT 10.x（libnvinfer.so.10 等）。系统装了 TRT 11.1 后，TRT 11 的 `libnvinfer.so.11` 内部 dlopen `"libnvinfer_plugin.so"`（无版本名）→ 经 libmpv 的 RPATH（→ third_party/nvvfx/lib）命中 VFX 的 TRT 10 plugin → ABI 崩溃（SIGSEGV in dlopen）。

**配套代码变更**（git 跟踪）：`src/mpv/video/filter/vsr_proc.c` 加载列表 `"libnvinfer.so"` → `"libnvinfer.so.10"`（同样 .10 版本化 plugin/onnxparser）——VFX 依赖链全版本化，不受影响。

**为什么不能删软链**：vsr_proc.c 曾显式 dlopen 无版本名（全路径）——直接删会让 VFX 拿到系统 TRT 11.1，破坏 VSR。改名 + 版本化列表双管齐下。

**install.sh 需同步**：复制 VFX 库到 ~/.local/lib/vsr-player 时应同样移除/改名无版本 TRT 软链（当前 install.sh 尚未处理——todo）。

**验证**：改名后 VSR benchmark 正常（--scale 2 帧数不变）；rife 不再崩溃。TRT 11 的 plugin dlopen 落到系统 /usr/lib（11.1）✅
