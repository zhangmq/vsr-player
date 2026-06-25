# 画质设置重设计 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重设计画质设置 popup：scale 增加关闭选项，新增独立强制降噪设置，使用 VFX SDK 原生参数值，移除冗余 VSR 开关，新增硬解/软解切换按钮。

**Architecture:** 自底向上：VSRProcessor → Player.h API → PlayerCore → PlayerViewModel → main.cpp CLI → overlay.qml。每个 task 可独立编译验证。

**Tech Stack:** C++20, Qt6 (QML/Qt Quick), NVIDIA VFX SDK, CUDA, FFmpeg

---

### Task 1: VSRProcessor — 删除 quality_to_vfx，参数改为 int

**Files:**
- Modify: `src/core/VSRProcessor.h:24-27,33,53`
- Modify: `src/core/VSRProcessor.cpp:15-31,121-149,336-344`

**说明：** VSRProcessor 是最底层，改动不影响任何调用方编译（调用方暂不修改，Task 3 中同步更新调用方）。

- [ ] **Step 1: 修改 VSRProcessor.h — quality 参数改为 int**

将 `Quality quality` 参数全部改为 `int quality`，删除 `quality_` 的 `Quality` 类型。

`src/core/VSRProcessor.h`:

```cpp
// L24: 修改 init 声明
bool init(int in_w, int in_h, int out_w, int out_h, int quality);

// L33: 修改 reconfigure 声明
bool reconfigure(int out_w, int out_h, int quality);

// L53: 修改 quality_ 字段类型
int quality_ = 3;  // default: High (VFX upscale value)
```

- [ ] **Step 2: 修改 VSRProcessor.cpp — 删除 quality_to_vfx 函数**

删除整个 `quality_to_vfx` 函数（L15-31）。

- [ ] **Step 3: 修改 VSRProcessor.cpp — init() 中 quality 参数直达 SDK**

在 `init()` 中（L121-149 区域），将：

```cpp
bool denoise = (in_w_ == out_w_ && in_h_ == out_h_);
unsigned int qv = quality_to_vfx(quality_, denoise);
pfn_NvVFX_SetU32(vsr_handle_, "QualityLevel", qv);
fprintf(stderr, "VSR: [%s] %dx%d→%dx%d quality=%u %s\n",
        denoise ? "DENOISE" : "UPSCALE",
        in_w_, in_h_, out_w_, out_h_, qv,
        denoise ? "(1:1 denoising)" : "(AI super-resolution)");
```

改为：

```cpp
bool denoise = (in_w_ == out_w_ && in_h_ == out_h_);
pfn_NvVFX_SetU32(vsr_handle_, "QualityLevel", (unsigned int)quality_);
fprintf(stderr, "VSR: [%s] %dx%d→%dx%d quality=%d %s\n",
        denoise ? "DENOISE" : "UPSCALE",
        in_w_, in_h_, out_w_, out_h_, quality_,
        denoise ? "(1:1 denoising)" : "(AI super-resolution)");
```

- [ ] **Step 4: 修改 VSRProcessor.cpp — reconfigure() 参数改为 int**

L336 的 `reconfigure` 签名改为：

```cpp
bool VSRProcessor::reconfigure(int out_w, int out_h, int quality) {
    fprintf(stderr, "VSR: reconfigure — in=%dx%d out=%dx%d quality=%d\n",
            in_w_, in_h_, out_w, out_h, quality);
    // ...
```

- [ ] **Step 5: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

预期：VSRProcessor 编译通过。PlayerCore 可能有调用不匹配的 warning/error（Task 3 修复），这是预期的。

- [ ] **Step 6: Commit**

```bash
git add src/core/VSRProcessor.h src/core/VSRProcessor.cpp
git commit -m "refactor: delete quality_to_vfx, pass VFX quality values directly to SDK"
```

---

### Task 2: Player.h API — 更新命令结构体

**Files:**
- Modify: `src/core/api/Player.h:70-82`

- [ ] **Step 1: CmdSetQuality.level 改为 int，新增 CmdSetDenoiseQuality**

`src/core/api/Player.h` L70-82:

```cpp
struct CmdSetQuality       { int level; };              // VFX quality value (1-4 upscale, 8-11 denoise)
struct CmdSetScale         { int scale; };              // -1=off, 0=auto, 2/3/4=locked
struct CmdSetVolume        { double value; };           // 0.0 - 1.0
struct CmdSetMute          { bool muted; };
struct CmdSetVsr           { bool enabled; };
struct CmdSetHwaccel       { bool enabled; };
struct CmdSetSpeed         { double speed; };
struct CmdSetDenoiseQuality { int level; };             // 新增：-1=off, 8-11=low-ultra
```

并更新 `PlayerCommand` variant（L78-82）：

```cpp
using PlayerCommand = std::variant<
    CmdPlay, CmdPause, CmdStop, CmdQuit,
    CmdLoadFile, CmdSeek, CmdResize,
    CmdSetQuality, CmdSetScale, CmdSetVolume, CmdSetMute,
    CmdSetVsr, CmdSetHwaccel, CmdSetSpeed, CmdSetDenoiseQuality>;
```

- [ ] **Step 2: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | grep -E "error|Error" | head -20
```

预期：variant 相关编译错误（PlayerCore.cpp 和 PlayerViewModel.cpp 中不完整的 visit）。Task 3/4 修复。

- [ ] **Step 3: Commit**

```bash
git add src/core/api/Player.h
git commit -m "feat: add CmdSetDenoiseQuality, change CmdSetQuality.level to int"
```

---

### Task 3: PlayerCore — 核心逻辑变更

**Files:**
- Modify: `src/core/PlayerCore.h:89,111,154,157`
- Modify: `src/core/PlayerCore.cpp:232,237,377,392-401,441,490,613-617,629-634,638-641,891-925,929-945`

- [ ] **Step 1: PlayerCore.h — 新增 denoise_quality_，修改 cmd_set_quality 签名**

`src/core/PlayerCore.h`:

```cpp
// L89: quality_ 字段类型改为 int
int quality_ = 3;  // VFX upscale quality (1-4, default HIGH=3)

// 新增 denoise_quality_ 字段（在 quality_ 下方）
int denoise_quality_ = -1;  // VFX denoise quality (-1=off, 8-11=low-ultra)

// L111: user_scale_ 默认值不变，注释更新
int user_scale_ = 0;  // -1=off, 0=auto, 2-4=locked

// L154: 修改 cmd_set_quality 签名
void cmd_set_quality(int q);

// L157: 新增 cmd_set_denoise_quality 声明
void cmd_set_denoise_quality(int d);
```

- [ ] **Step 2: PlayerCore.cpp — dispatch() 中 is_light 新增 CmdSetDenoiseQuality**

在 `is_light()` 中（L138-153），找到 `CmdSetHwaccel` 一行，在其后添加：

```cpp
if constexpr (std::is_same_v<T, CmdSetDenoiseQuality>) return true;
```

- [ ] **Step 3: PlayerCore.cpp — dispatch() 中 heavy 分支新增 CmdSetDenoiseQuality 合并到 target_state**

找到 `dispatch()` 中 target_state_.merge(cmd) 调用处的 heavy 分支（L252-255 区域），确保 `CmdSetDenoiseQuality` 不被漏掉——它会被 `is_light` 返回 true 直接执行（step 2 已添加为 light 命令）。

实际上 `cmd_set_denoise_quality` 应该是 light 命令（它只修改一个 flag 并在 scale=1 时 reconfigure VSR），不走 target_state 合并路径。这和 `cmd_set_quality` 的行为一致。Step 2 已处理为 light。

- [ ] **Step 4: PlayerCore.cpp — 修改 cmd_set_quality 签名**

L613，将：

```cpp
void PlayerCore::cmd_set_quality(Quality q) {
```

改为：

```cpp
void PlayerCore::cmd_set_quality(int q) {
    quality_ = q;
    if (vsr_ && vsr_->is_ready())
        vsr_->reconfigure(vsr_w_, vsr_h_, q);
}
```

注意移除了原来的 `Quality` 类型引用。

- [ ] **Step 5: PlayerCore.cpp — 新增 cmd_set_denoise_quality**

在 `cmd_set_quality` 之后添加：

```cpp
void PlayerCore::cmd_set_denoise_quality(int d) {
    denoise_quality_ = d;
    // Only takes effect when effective scale is 1 (denoise mode)
    int effective = current_scale_;
    if (effective == 1)
        reconfigure_vsr();
}
```

- [ ] **Step 6: PlayerCore.cpp — 修改 cmd_set_scale 范围检查**

L893-894，将：

```cpp
if (s < 0 || s > 4) return;
```

改为：

```cpp
if (s < -1 || s > 4 || s == 1) return;
```

- [ ] **Step 7: PlayerCore.cpp — 修改 reconfigure_vsr()**

L929-945，替换整个函数：

```cpp
void PlayerCore::reconfigure_vsr() {
    // Determine whether VSR should be active
    bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
    if (!vsr_active) {
        vsr_.reset();
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
        return;
    }

    int effective = current_scale_;
    if (effective > 1) {
        // Upscale mode
        vsr_w_ = video_w_ * effective;
        vsr_h_ = video_h_ * effective;
        if (vsr_ && vsr_->is_ready()) {
            vsr_->reconfigure(vsr_w_, vsr_h_, quality_);
        } else {
            vsr_ = std::make_unique<VSRProcessor>();
            vsr_->set_stream(cuda_stream_);
            if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, quality_)) {
                fprintf(stderr, "PlayerCore: VSR init (upscale) failed\n");
                vsr_.reset();
            }
        }
    } else if (effective == 1 && denoise_quality_ != -1) {
        // Denoise mode
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
        if (vsr_ && vsr_->is_ready()) {
            vsr_->reconfigure(vsr_w_, vsr_h_, denoise_quality_);
        } else {
            vsr_ = std::make_unique<VSRProcessor>();
            vsr_->set_stream(cuda_stream_);
            if (!vsr_->init(video_w_, video_h_, vsr_w_, vsr_h_, denoise_quality_)) {
                fprintf(stderr, "PlayerCore: VSR init (denoise) failed\n");
                vsr_.reset();
            }
        }
    } else {
        // VSR off (scale=1 but denoise off)
        vsr_.reset();
        vsr_w_ = video_w_;
        vsr_h_ = video_h_;
    }
}
```

- [ ] **Step 8: PlayerCore.cpp — execute() 中的 VSR 激活检查和初始化**

在 `execute()` 的 Path A 中（L388-402 区域），替换 VSR 初始化逻辑：

```cpp
// VSR init
bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
if (vsr_ && !vsr_active) {
    CLOG("executeA: vsr disabled (scale=off, denoise=off)");
    vsr_.reset();
}
if (vsr_active && !vsr_) {
    vsr_ = std::make_unique<VSRProcessor>();
    vsr_->set_stream(cuda_stream_);
    int vsr_ow = video_w_ * scale;
    int vsr_oh = video_h_ * scale;
    CLOG("executeA: vsr init %dx%d->%dx%d", video_w_, video_h_, vsr_ow, vsr_oh);
    int vsr_quality = (scale > 1) ? (int)quality_ : denoise_quality_;
    if (!vsr_->init(video_w_, video_h_, vsr_ow, vsr_oh, vsr_quality)) {
        fprintf(stderr, "PlayerCore: VSR init failed\n");
        vsr_.reset();
    }
}
```

L441 附近，`quality_ = quality;` 改为 `quality_ = snapshot.quality >= 0 ? snapshot.quality : quality_;`（但 quality 现在已经是 int，默认为 -1）。需要修改 TargetState 的 quality 默认值。

在 `TargetState` 的 `clear()` 中（PlayerCore.h L36），保持 `quality = -1` 不变。

- [ ] **Step 9: PlayerCore.cpp — 修改 compute_adaptive_scale 中的 use_vsr_ 引用**

L662-677，`compute_adaptive_scale` 不再依赖 `use_vsr_`，改为检查 VSR 激活条件：

```cpp
int PlayerCore::compute_adaptive_scale(int phys_w, int phys_h,
                                       int video_w, int video_h) const {
    bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);
    if (!vsr_active || video_w <= 0 || video_h <= 0) return 1;
    // ... rest unchanged
```

- [ ] **Step 10: PlayerCore.cpp — visit 中 cmd_set_quality 改为 lambda 调用**

dispatch() 中 light 命令的 visit（L225-237），找到 `CmdSetQuality` 分支。当前 L232：
```cpp
else if constexpr (std::is_same_v<T, CmdSetQuality>) cmd_set_quality(arg.level);
```
因为 `arg.level` 现在是 `int` 而不是 `Quality`，此行自然兼容。

但需新增 `CmdSetDenoiseQuality` 的分发：
```cpp
else if constexpr (std::is_same_v<T, CmdSetDenoiseQuality>) cmd_set_denoise_quality(arg.level);
```

- [ ] **Step 11: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | tail -30
```

预期：编译通过。

- [ ] **Step 12: Commit**

```bash
git add src/core/PlayerCore.h src/core/PlayerCore.cpp
git commit -m "feat: add denoise_quality, rework VSR activation/reconfigure logic"
```

---

### Task 4: PlayerViewModel — 属性类型变更 + 新 slot

**Files:**
- Modify: `src/client/PlayerViewModel.h:30-33,47,50-53,73-74,80,85-86,89,122-123`
- Modify: `src/client/PlayerViewModel.cpp:83-113,149-162,165-183`

- [ ] **Step 1: PlayerViewModel.h — 属性类型变更**

```cpp
// L30: quality 从 QString 改为 int
Q_PROPERTY(int quality READ quality NOTIFY qualityChanged)

// L32: scale 不变（仍是 int）
Q_PROPERTY(int scale READ scale NOTIFY scaleChanged)

// 新增 denoiseQuality 属性（在 scale 下方）
Q_PROPERTY(int denoiseQuality READ denoiseQuality NOTIFY denoiseQualityChanged)

// L47: quality() 返回类型改为 int
int quality() const     { return quality_; }

// L50: vsrActive() 改为派生属性
bool vsrActive() const { return scale_ != -1 || denoise_quality_ != -1; }

// L53: 新增 denoiseQuality() 访问器
int denoiseQuality() const { return denoise_quality_; }

// L73: setQuality 签名改为 int
void setQuality(int q);

// L80: toggleVsr() 删除
// (移除整行)

// 新增 setDenoiseQuality 和 toggleHwaccel slot（在 setScale 下方）
void setDenoiseQuality(int d);
void toggleHwaccel();
```

- [ ] **Step 2: PlayerViewModel.h — 信号和字段**

```cpp
// 新增信号（在 qualityChanged 下方）
void denoiseQualityChanged();

// 新增字段（在 quality_ 下方）
int denoise_quality_ = -1;

// L122: quality_ 类型改为 int
int quality_ = 3;  // High

// 删除 vsrActive_ 存储字段（L115）
// bool vsrActive_ = false;  ← 删除
```

- [ ] **Step 3: PlayerViewModel.cpp — 删除 toggleVsr()**

删除整个 `toggleVsr()` 函数（L101-106）。

- [ ] **Step 4: PlayerViewModel.cpp — 修改 setQuality**

替换 L83-92：

```cpp
void PlayerViewModel::setQuality(int q) {
    if (!player_) return;
    if (q < 1 || q > 4) return;
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
    player_->send_command(CmdSetQuality{q});
}
```

- [ ] **Step 5: PlayerViewModel.cpp — 新增 setDenoiseQuality**

在 `setScale` 之后添加：

```cpp
void PlayerViewModel::setDenoiseQuality(int d) {
    if (!player_) return;
    // Valid values: -1 (off), 8 (low), 9 (medium), 10 (high), 11 (ultra)
    if (d != -1 && (d < 8 || d > 11)) return;
    if (denoise_quality_ != d) { denoise_quality_ = d; emit denoiseQualityChanged(); }
    bool wasActive = (scale_ != -1 || denoise_quality_ != -1);
    player_->send_command(CmdSetDenoiseQuality{d});
    // vsrActive may change
    bool nowActive = (scale_ != -1 || d != -1);
    if (wasActive != nowActive) emit vsrActiveChanged();
}
```

- [ ] **Step 6: PlayerViewModel.cpp — 新增 toggleHwaccel**

在 `toggleMute` 之后添加：

```cpp
void PlayerViewModel::toggleHwaccel() {
    if (!player_) return;
    hwDecoding_ = !hwDecoding_;
    emit hwDecodingChanged();
    player_->send_command(CmdSetHwaccel{hwDecoding_});
}
```

- [ ] **Step 7: PlayerViewModel.cpp — 修改 updateQuality 接受 int**

L157-158，将 `updateQuality(const QString& q)` 改为 `updateQuality(int q)`：

```cpp
void PlayerViewModel::updateQuality(int q) {
    if (quality_ != q) { quality_ = q; emit qualityChanged(); }
}
```

同时更新 PlayerViewModel.h 中的声明（L88）。

- [ ] **Step 8: PlayerViewModel.cpp — 修改 onPending 中的 pending_quality_ 处理**

L175-177，`pending_quality_` 保持 int 类型（已经是 int），无需改动。但需要确认 `TargetState.quality` 也是 int——已在 Task 3 中改为 int。

- [ ] **Step 9: PlayerViewModel.cpp — 修改 setScale 中 vsrActiveChanged 的 emit**

L96-99，当前 `setScale` 函数中：

```cpp
void PlayerViewModel::setScale(int s) {
    if (!player_) return;
    if (s < -1 || s > 4 || s == 1) return;
    if (scale_ != s) { scale_ = s; emit scaleChanged(); }
    bool wasActive = (scale_ != -1 || denoise_quality_ != -1);
    player_->send_command(CmdSetScale{s});
    bool nowActive = (s != -1 || denoise_quality_ != -1);
    if (wasActive != nowActive) emit vsrActiveChanged();
}
```

- [ ] **Step 10: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | tail -30
```

预期：编译通过。

- [ ] **Step 11: Commit**

```bash
git add src/client/PlayerViewModel.h src/client/PlayerViewModel.cpp
git commit -m "feat: refactor ViewModel — int quality/scale, add denoiseQuality, toggleHwaccel, remove toggleVsr"
```

---

### Task 5: main.cpp — CLI 参数映射

**Files:**
- Modify: `src/client/main.cpp`

- [ ] **Step 1: 读取 main.cpp 以定位 CLI 解析代码**

```bash
grep -n "no-vsr\|--quality\|use_vsr\|CmdSetQuality\|CmdSetScale\|no_hwaccel" src/client/main.cpp
```

- [ ] **Step 2: 更新 CLI 参数解析**

将 `--no-vsr` 选项移除。将 `--quality` 的值从 Quality 枚举映射改为 int 映射。

添加 `--denoise` 和 `--scale` 参数。

修改 Player::initialize 调用，移除 `use_vsr` 参数（该参数不再使用——VSR 激活由 scale/denoise 决定）。

字符串映射：

```cpp
// --quality
if (parser.isSet("quality")) {
    QString q = parser.value("quality").toLower();
    int quality = 3;  // default high
    if (q == "low") quality = 1;
    else if (q == "medium") quality = 2;
    else if (q == "high") quality = 3;
    else if (q == "ultra") quality = 4;
    // quality_ will be set via CmdSetQuality after init
}

// --denoise (new)
if (parser.isSet("denoise")) {
    QString d = parser.value("denoise").toLower();
    int denoise = -1;  // default off
    if (d == "off") denoise = -1;
    else if (d == "low") denoise = 8;
    else if (d == "medium") denoise = 9;
    else if (d == "high") denoise = 10;
    else if (d == "ultra") denoise = 11;
}

// --scale (new)
if (parser.isSet("scale")) {
    QString s = parser.value("scale").toLower();
    int scale = 0;  // default auto
    if (s == "off" || s == "关闭") scale = -1;
    else if (s == "auto" || s == "自动") scale = 0;
    else if (s == "2x" || s == "2") scale = 2;
    else if (s == "3x" || s == "3") scale = 3;
    else if (s == "4x" || s == "4") scale = 4;
}
```

同时需要更新 `Player::initialize()` 调用处——`use_vsr` 参数改为 `true`（VSR 激活由 scale/denoise 在运行时决定）。

- [ ] **Step 3: 移除 --no-vsr 参数定义**

在 QCommandLineParser 的选项定义中移除 `--no-vsr`。

- [ ] **Step 4: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add src/client/main.cpp
git commit -m "feat: update CLI — remove --no-vsr, add --denoise and --scale with string mapping"
```

---

### Task 6: overlay.qml — 重写 Quality Popup + 新增硬解按钮

**Files:**
- Modify: `src/client/ui/overlay.qml:303-364`（quality popup 区域）
- Modify: `src/client/ui/overlay.qml:233-253`（bottom bar 右侧按钮组）

- [ ] **Step 1: 替换 Quality Popup（L303-364）**

删除当前 qualityPopup 的全部内容，替换为：

```qml
    // ══════════════════════════════════════════════════════════════════
    // Quality Popup
    // ══════════════════════════════════════════════════════════════════

    Popup {
        id: qualityPopup
        x: { var p = qualBtn.mapToItem(root, 0, 0)
             return Math.min(p.x + qualBtn.width/2 - width/2, root.width - width - 8) }
        y: bottomBar.y - height - 8
        width: 360; padding: 16
        modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

        Column {
            spacing: 10; anchors { left: parent.left; right: parent.right }

            // ── Scale ──
            Text { text: "Scale"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: [
                        {label: "关闭", value: -1},
                        {label: "自动", value: 0},
                        {label: "2×", value: 2},
                        {label: "3×", value: 3},
                        {label: "4×", value: 4}
                    ]
                    Rectangle {
                        width: 56; height: 32; radius: 4
                        color: sm2.containsMouse ? "#33ffffff"
                             : (viewModel.scale === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.scale === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: sm2; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setScale(modelData.value); qualityPopup.close() } }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

            // ── 超分质量 ──
            Text { text: "超分质量"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: [
                        {label: "Low", value: 1},
                        {label: "Medium", value: 2},
                        {label: "High", value: 3},
                        {label: "Ultra", value: 4}
                    ]
                    Rectangle {
                        width: 70; height: 32; radius: 4
                        color: qm2.containsMouse ? "#33ffffff"
                             : (viewModel.quality === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.quality === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: qm2; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setQuality(modelData.value); qualityPopup.close() } }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

            // ── 强制降噪 ──
            Text { text: "强制降噪（scale=1时生效）"; color: "#b0b0b0"; font.pixelSize: 13 }
            Row { spacing: 6; anchors { left: parent.left; right: parent.right }
                Repeater {
                    model: [
                        {label: "关闭", value: -1},
                        {label: "Low", value: 8},
                        {label: "Medium", value: 9},
                        {label: "High", value: 10},
                        {label: "Ultra", value: 11}
                    ]
                    Rectangle {
                        width: 56; height: 32; radius: 4
                        color: dm2.containsMouse ? "#33ffffff"
                             : (viewModel.denoiseQuality === modelData.value ? "#33ffcc00" : "transparent")
                        Text { anchors.centerIn: parent
                            text: modelData.label
                            color: viewModel.denoiseQuality === modelData.value ? "#ffcc00" : "#e0e0e0"
                            font.pixelSize: 13 }
                        MouseArea { id: dm2; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { viewModel.setDenoiseQuality(modelData.value); qualityPopup.close() } }
                    }
                }
            }
        }
    }
```

- [ ] **Step 2: 底部栏新增硬解/软解按钮**

在 bottomBar 右侧 Row 中（L233-253），在 playlistBtn 之前插入：

```qml
            IconButton { id: hwBtn; codepoint: ""; size: 22
                tooltip: viewModel.hwDecoding ? "硬解 (点击切换软解)" : "软解 (点击切换硬解)"
                highlighted: viewModel.hwDecoding
                onClicked: viewModel.toggleHwaccel() }
```

- [ ] **Step 3: 更新 auto-hide timer 中的 popup 引用**

L63，timer 中已有 `qualityPopup.visible` 检查，无需改动（popup id 不变）。

- [ ] **Step 4: 编译 + 运行测试**

```bash
cd build && make -j$(nproc) && echo "=== BUILD OK ==="
```

预期：编译通过。

手动测试清单：
1. 点击画质按钮 → popup 显示三个区块
2. 点击 Scale 各选项 → 高亮切换，popup 关闭
3. 点击超分质量各选项 → 高亮切换，popup 关闭
4. 点击强制降噪各选项 → 高亮切换，popup 关闭
5. Scale=关闭 + 强制降噪=关闭 → vsrActive=false
6. Scale=关闭 + 强制降噪=High → vsrActive=true（降噪模式）
7. Scale=2× → vsrActive=true（超分模式）
8. 硬解按钮点击切换，tooltip 相应更新

- [ ] **Step 5: Commit**

```bash
git add src/client/ui/overlay.qml
git commit -m "feat: redesign quality popup — three independent sections, add hwaccel toggle button"
```

---

### Task 7: 事件回调同步 — 修复 event → ViewModel 的类型不匹配

**Files:**
- Modify: `src/client/main.cpp`（event callback 处）

- [ ] **Step 1: 修改事件回调中的 updateQuality 调用**

`main.cpp` 中的 event callback 调用 `viewModel->updateQuality(...)`。因为 `updateQuality` 参数从 `QString` 改为 `int`，需要更新调用处。

搜索 main.cpp 中所有 `updateQuality` 调用，将 `QString::number((int)e.quality)` 或类似的字符串转换改为直接传 int：

```cpp
// 旧（QString 参数）:
viewModel->updateQuality(QString::number((int)e.quality));

// 新（int 参数）:
viewModel->updateQuality((int)e.quality);
```

同时需要更新 `PlayerEvent::quality` 字段类型（Player.h L108）从 `Quality` 改为 `int`：

```cpp
int quality = 3;  // was: Quality quality = Quality::HIGH;
```

- [ ] **Step 2: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

预期：编译通过，无类型不匹配。

- [ ] **Step 3: Commit**

```bash
git add src/client/main.cpp src/core/api/Player.h
git commit -m "fix: update event callback for int quality type"
```

---

### Task 8: 全局清理 — 移除 use_vsr_ 残留引用 + Quality 枚举清理

**Files:**
- Modify: `src/core/PlayerCore.h:87,88`
- Modify: `src/core/PlayerCore.cpp:80,88,392,398,662,664,770,771,930`
- Modify: `src/core/api/Player.h:30`
- Modify: `src/client/main.cpp`

- [ ] **Step 1: 清理 use_vsr_ 字段**

`use_vsr_` 字段在 PlayerCore 中多处使用但功能已被 `!(scale==-1 && denoise==-1)` 替代。

PlayerCore.h L87-88：
```cpp
// 删除 use_vsr_ 字段
// bool use_vsr_ = true;  ← 删除
```

PlayerCore.cpp：搜索所有 `use_vsr_` 引用，改为使用 VSR 激活条件 `!(user_scale_ == -1 && denoise_quality_ == -1)`：

```bash
grep -n "use_vsr_" src/core/PlayerCore.cpp
```

逐一替换为局部变量 `bool vsr_active = !(user_scale_ == -1 && denoise_quality_ == -1);`

- [ ] **Step 2: 删除 Player::initialize 的 use_vsr 参数**

Player.h L145：
```cpp
// 旧：
bool initialize(IVulkanContext* vk, ...,
    bool use_vsr = true, Quality quality = Quality::HIGH, bool no_hwaccel = false) = 0;

// 新：
bool initialize(IVulkanContext* vk, ...,
    int quality = 3, bool no_hwaccel = false) = 0;
```

PlayerCore.h L65-67 同步更新声明。

main.cpp 中 `player_->initialize(...)` 调用处移除 `use_vsr` 参数。

- [ ] **Step 3: Quality 枚举保留但标记 deprecated**

Player.h L30：`Quality` 枚举保留定义（避免大面积 break），但不再在核心逻辑中使用。

- [ ] **Step 4: 编译验证**

```bash
cd build && make -j$(nproc) 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add src/core/PlayerCore.h src/core/PlayerCore.cpp src/core/api/Player.h src/client/main.cpp
git commit -m "refactor: remove use_vsr_ flag, simplify initialize() signature"
```

---

## 验证清单

完成所有 Task 后：

1. **编译**：`cd build && make -j$(nproc)` 无错误
2. **CLI 参数**：`./build/vsr-player --quality high --denoise off --scale auto <video>` 正常启动
3. **UI 交互**：
   - 画质 popup 三区块独立切换
   - 强制降噪选项在 scale=1（关闭或自动=1）时生效
   - 硬解/软解按钮点击切换，tooltip 更新
4. **VSR 行为**：
   - scale=关闭 + 强制降噪=关闭 → VSR 不初始化（帧直通）
   - scale=关闭 + 强制降噪=High → VSR 降噪模式
   - scale=自动/2×/3×/4× → VSR 超分模式
