# 画质设置重设计

> 日期：2026-06-26 | 状态：已确认

## 目标

重设计画质设置 popup，新增独立强制降噪选项，使用 VFX SDK 原生参数值消除转换层，移除 UI 中冗余的 VSR 开关。

## 数据编码

### scale（user_scale_）

| 值 | 标签 | 含义 |
|----|------|------|
| -1 | 关闭 | 1× 无放大 |
| 0 | 自动 | 按窗口自适应（结果 1-4） |
| 2 | 2× | 锁定 |
| 3 | 3× | 锁定 |
| 4 | 4× | 锁定 |

### quality（quality_）

| 值 | 含义 | 备注 |
|----|------|------|
| 0 | 自动 | 预留，暂不实现 |
| 1 | Low | VFX upscale 值 |
| 2 | Medium | VFX upscale 值 |
| 3 | High | **默认** |
| 4 | Ultra | VFX upscale 值 |

### denoise_quality_（新增）

| 值 | 含义 | 备注 |
|----|------|------|
| -1 | 关闭 | **默认** |
| 0 | 自动 | 预留，暂不实现 |
| 8 | Low | VFX denoise 值 |
| 9 | Medium | VFX denoise 值 |
| 10 | High | VFX denoise 值 |
| 11 | Ultra | VFX denoise 值 |

## VSR 逻辑

### 激活条件

```
vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1)
```

只有当 scale=关闭 且 强制降噪=关闭 时，VSR 才彻底关闭。

### 模式选择

```
effective_scale = (user_scale_ == 0) ? adaptive_result : (user_scale_ == -1 ? 1 : user_scale_)

if effective_scale > 1:
    mode = upscale, 参数 = quality_
elif effective_scale == 1 && denoise_quality_ != -1:
    mode = denoise, 参数 = denoise_quality_
else:
    VSR 不初始化
```

## 涉及文件

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `src/client/ui/overlay.qml` | 重写 | quality popup 三区块 |
| `src/client/PlayerViewModel.h` | 修改 | scale/quality 改 int，新增 denoiseQuality，移除 vsrActive 存储 |
| `src/client/PlayerViewModel.cpp` | 修改 | setQuality 改 int 参数，新增 setDenoiseQuality，删除 toggleVsr |
| `src/client/main.cpp` | 修改 | 命令行参数映射更新 |
| `src/core/api/Player.h` | 修改 | CmdSetQuality.level 改 int，新增 CmdSetDenoiseQuality |
| `src/core/PlayerCore.h` | 修改 | 新增 denoise_quality_，cmd_set_quality 改 int 参数 |
| `src/core/PlayerCore.cpp` | 修改 | 新增 cmd_set_denoise_quality，reconfigure_vsr 三路径逻辑 |
| `src/core/VSRProcessor.h` | 修改 | quality 参数改 int |
| `src/core/VSRProcessor.cpp` | 修改 | 删除 quality_to_vfx，参数直达 SDK |

## UI 设计

### Quality Popup 布局

```
┌─────────────────────────────────┐
│  画质                           │
│                                 │
│  Scale                          │
│  [关闭] [自动] [2×] [3×] [4×]   │
│                                 │
│  ─────────────────────────────  │
│                                 │
│  超分质量                       │
│  [Low] [Medium] [High] [Ultra] │
│                                 │
│  ─────────────────────────────  │
│                                 │
│  强制降噪   (scale=1时生效)     │
│  [关闭] [Low] [Medium] [High] [Ultra] │
└─────────────────────────────────┘
```

### 规则

- 三个区块独立操作，无动态联动
- 强制降噪始终可操作，标签提示 "scale=1时生效"
- 移除 vsrSwitch 开关（被 scale=关闭 替代）
- 移除 "Denoise / AI Super Resolution" 动态标签
- 每项当前值高亮（黄色 #ffcc00）

### Bottom Bar 新增按钮

在右侧按钮组（volume/quality/speed 之后）新增硬解/软解切换按钮：

```qml
IconButton { codepoint: ""; size: 22
    tooltip: viewModel.hwDecoding ? "硬解 (点击切换软解)" : "软解 (点击切换硬解)"
    highlighted: viewModel.hwDecoding
    onClicked: viewModel.toggleHwaccel() }
```

点击直接切换，无需 popup。切换在下一个文件加载时生效。

### QML 数据模型

```qml
// Scale
model: [
    {label: "关闭", value: -1},
    {label: "自动", value: 0},
    {label: "2×", value: 2},
    {label: "3×", value: 3},
    {label: "4×", value: 4}
]

// 超分质量
model: [
    {label: "Low", value: 1},
    {label: "Medium", value: 2},
    {label: "High", value: 3},
    {label: "Ultra", value: 4}
]

// 强制降噪
model: [
    {label: "关闭", value: -1},
    {label: "Low", value: 8},
    {label: "Medium", value: 9},
    {label: "High", value: 10},
    {label: "Ultra", value: 11}
]
```

## PlayerViewModel 变更

### 属性

| 属性 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| scale | int | 0 | 范围 -1/0/2/3/4 |
| quality | int | 3 | 范围 1-4 |
| denoiseQuality | int | -1 | 新增，范围 -1/8/9/10/11 |
| vsrActive | bool | 派生 | 改为 `scale != -1 \|\| denoiseQuality != -1` |

### Slot

| 方法 | 说明 |
|------|------|
| setScale(int s) | 范围扩展 |
| setQuality(int q) | 参数从 QString 改为 int |
| setDenoiseQuality(int d) | 新增 |
| toggleHwaccel() | 新增，切换硬解/软解 |
| toggleVsr() | 删除 |

## Core 变更

### PlayerCore

- `user_scale_` 范围扩展为 -1/0/2/3/4
- 新增 `denoise_quality_` 字段（默认 -1）
- `cmd_set_quality(int q)` 参数从 Quality 枚举改为 int
- 新增 `cmd_set_denoise_quality(int d)`
- `reconfigure_vsr()` 三路径：upscale / denoise / off
- `compute_adaptive_scale()` 及 VSR 初始化处用 `!(scale==-1 && denoise==-1)` 判断激活

### VSRProcessor

- `init(int in_w, int in_h, int out_w, int out_h, int quality)` — quality 参数改为 int
- `reconfigure(int out_w, int out_h, int quality)` — 同上
- 删除 `quality_to_vfx()` 函数
- quality 值直接传给 `NvVFX_SetU32(handle, "QualityLevel", quality)`

### API 命令

```cpp
struct CmdSetQuality       { int level; };       // Quality 枚举 → int
struct CmdSetDenoiseQuality { int level; };       // 新增
// CmdSetVsr 保留但不暴露给 UI
```

## 命令行参数

```
--quality high       → quality_ = 3
--quality ultra      → quality_ = 4
--denoise off        → denoise_quality_ = -1
--denoise high       → denoise_quality_ = 10
--scale auto         → user_scale_ = 0
--scale 2x           → user_scale_ = 2
--no-hwaccel         → 保留不变
```

字符串映射表（main.cpp 中处理）：

| 参数 | 可选值 | 默认值 |
|------|--------|--------|
| `--quality` | low, medium, high, ultra | high |
| `--denoise` | off, low, medium, high, ultra | off |
| `--scale` | off, auto, 2x, 3x, 4x | auto |

移除 `--no-vsr` 参数（由 `--scale off --denoise off` 替代）。

## 硬解/软解切换按钮

在 bottom bar 右侧按钮组新增一个图标按钮，点击直接切换硬解/软解（无需 popup）。

### ViewModel

- 新增 `toggleHwaccel()` slot：
  ```cpp
  void toggleHwaccel() {
      if (!player_) return;
      bool newVal = !hwDecoding_;
      player_->send_command(CmdSetHwaccel{!newVal});  // 传的是 enable
      // no_hwaccel flag 在 core 中反转，下一个 LOAD_FILE 生效
  }
  ```
  ——注意：`CmdSetHwaccel{bool}` 的语义是 enable hwaccel。`no_hwaccel_` 在 core 内部取反。
  
  但由于 hwaccel 切换实际在下一个文件加载时才生效，可能需要通过 event 更新状态。当前 `updateHwDecoding` 在 VIDEO_INFO 事件中回调。为支持即时 UI 反馈，slot 内乐观更新：
  ```cpp
  hwDecoding_ = !hwDecoding_;
  emit hwDecodingChanged();
  ```

### QML

Bottom bar 右侧 Row 新增按钮：

```qml
IconButton { codepoint: ""; size: 22
    tooltip: viewModel.hwDecoding ? "硬解 (点击切换软解)" : "软解 (点击切换硬解)"
    highlighted: viewModel.hwDecoding
    onClicked: viewModel.toggleHwaccel() }
```

### Core

不需要改动。`cmd_set_hwaccel(bool enabled)` 已实现，设置 `no_hwaccel_ = !enabled`，在下一个 LOAD_FILE 时生效。

## 默认值汇总

| 设置 | 默认值 | 语义 |
|------|--------|------|
| scale | 0 | 自动 |
| quality | 3 | High |
| denoise_quality | -1 | 关闭 |
