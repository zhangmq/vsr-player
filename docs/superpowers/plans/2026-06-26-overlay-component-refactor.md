# Overlay QML Component Refactor — Implementation Plan

> **For agentic workers:** Use this plan task-by-task. Each task extracts one component from overlay.qml. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `src/client/ui/overlay.qml` (587 lines) into 10 component files. Zero functional change.

**Architecture:** Each component file is a verbatim extraction of its overlay.qml block, with `property`/`signal` declarations added at the top, and `id`-based cross-references replaced with root-property/signal access. `overlay.qml` instantiates components with property bindings and signal wiring.

**Tech Stack:** Qt 6 QML (QtQuick, QtQuick.Controls)

**Required Skills (loaded per task):**
| Skill | When | Purpose |
|---|---|---|
| `qt-development-skills:qt-qml` | Every implementer agent | Enforce Qt 6 QML rules: no version numbers on imports, no `anchors`+`Layout.*` mixing, `required property` for model roles, use `Item` not transparent `Rectangle`, avoid `clip: true` unless needed, wrap user strings in `qsTr()`, etc. |
| `qt-development-skills:qt-qml-review` | Every code-quality reviewer agent | Check extracted component against full QML rule checklist; report violations only |

---

## ⛔ HARD CONSTRAINT

This is pure extraction. Every line producing visual/behavioral output is preserved verbatim. The ONLY allowed changes within a component body:

1. Adding `property`/`signal`/`readonly property` at the top of the file
2. Replacing `externalId.something` with `root.property` or `root.signal()` (where `externalId` no longer exists after extraction)
3. Adding `import "components"` to overlay.qml's imports

---

## Task Order (dependency-driven)

**Per-task skill protocol:**
- **Implementer agent:** invoke `qt-development-skills:qt-qml` before writing QML files
- **Code-quality reviewer agent:** invoke `qt-development-skills:qt-qml-review`, report violations only
- Key rules to watch: no version numbers on imports, `Item` over transparent `Rectangle`, `readonly property` for computed values, `signal` for upward communication, no `anchors`+`Layout.*` mixing

1. **IconButton** — leaf component used by BottomBar, VolumePopup, PlaylistPanel
2. **OsdOverlay** — leaf, no dependencies
3. **TopBar** — leaf, no dependencies
4. **CenterPlayBtn** — leaf, no dependencies
5. **ProgressSlider** — leaf, anchors to `bottomBar.id` (still exists in overlay.qml at this point)
6. **BottomBar** — exposes `volumeBtnCenterX`/`qualityBtnCenterX`/`speedBtnCenterX` needed by popups
7. **VolumePopup** — uses IconButton + bottomBar centerX
8. **QualityPopup** — uses bottomBar centerX
9. **SpeedPopup** — uses bottomBar centerX
10. **PlaylistPanel** — uses IconButton internally

---

### Task 1: Extract IconButton

**Files:**
- Create: `src/client/ui/components/IconButton.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Create components directory**

```bash
mkdir -p src/client/ui/components
```

- [ ] **Step 2: Write components/IconButton.qml**

```qml
import QtQuick
import QtQuick.Controls

Item {
    id: root
    property string codepoint
    property real size: 22
    property string tooltip: ""
    property bool highlighted: false
    property string label: ""
    signal clicked()
    implicitWidth: label ? label.length * 12 + 20 : size + 16
    implicitHeight: size + 16

    Rectangle {
        id: ibBg; anchors.fill: parent; radius: 4
        color: ibHover.hovered || root.highlighted ? "#33ffffff" : "transparent"
        Behavior on color { ColorAnimation { duration: 150 } }
    }
    Text {
        anchors.centerIn: parent
        font.family: label ? root.font.family : iconFont
        font.pixelSize: label ? size - 7 : size
        text: label ? label : codepoint
        color: ibHover.hovered || root.highlighted ? "#ffffff" : "#c8c8c8"
        Behavior on color { ColorAnimation { duration: 150 } }
        renderType: Text.NativeRendering
    }
    MouseArea {
        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
        hoverEnabled: true; onClicked: root.clicked()
    }
    HoverHandler { id: ibHover }
    ToolTip {
        visible: ibHover.hovered && tooltip !== ""; text: tooltip
        delay: 600; font.pixelSize: 11
        background: Rectangle { color: "#d9111111"; radius: 4; border { width: 1; color: "#22ffffff" } }
        contentItem: Text { text: tooltip; color: "#e0e0e0"; font.pixelSize: 11 }
    }
}
```

- [ ] **Step 3: Update overlay.qml**

Add `import "components"` after line 2 (`import QtQuick.Controls`). Remove lines 17–50 — the entire `component IconButton: Item { ... }` block (from the blank line before `component` through the closing `}` of the component, inclusive).

- [ ] **Step 4: Build and verify**

```bash
make -j$(nproc)
```

- [ ] **Step 5: Commit**

```bash
git add src/client/ui/components/IconButton.qml src/client/ui/overlay.qml
git commit -m "refactor: extract IconButton to components/IconButton.qml"
```

---

### Task 2: Extract OsdOverlay

**Files:**
- Create: `src/client/ui/OsdOverlay.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write OsdOverlay.qml**

```qml
import QtQuick

Item {
    id: root
    property bool osdVisible: false
    property string osdText: ""

    Rectangle {
        anchors { left: parent.left; top: parent.top; leftMargin: 16; topMargin: 64 }
        color: "#99000000"; radius: 6
        opacity: root.osdVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Text {
            text: root.osdText
            color: "#e0e0e0"
            font.family: "monospace"; font.pixelSize: 12
            lineHeight: 1.4
            leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
            renderType: Text.NativeRendering
        }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace lines 121–139 (from `═══ OSD Overlay` comment through the closing `}` of the OSD Rectangle) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // OSD Overlay (Tab toggle)
    // ══════════════════════════════════════════════════════════════════

    OsdOverlay {
        osdVisible: viewModel.osdVisible
        osdText: viewModel.osdText
    }
```

- [ ] **Step 3: Build and verify, then commit**

```bash
make -j$(nproc) && git add src/client/ui/OsdOverlay.qml src/client/ui/overlay.qml && git commit -m "refactor: extract OsdOverlay"
```

---

### Task 3: Extract TopBar

**Files:**
- Create: `src/client/ui/TopBar.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write TopBar.qml**

```qml
import QtQuick

Item {
    id: root
    property string videoInfo: ""
    property bool overlaysVisible: true
    implicitHeight: 48

    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#cc000000" }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Text {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            font.pixelSize: 14; elide: Text.ElideRight
            color: "#e0e0e0"
            text: {
                if (root.videoInfo) return root.videoInfo
                return "VSR Player"
            }
        }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Top Bar` comment block + `Rectangle { id: topBar ... }` (lines 69–93) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Top Bar
    // ══════════════════════════════════════════════════════════════════

    TopBar {
        id: topBar
        anchors { left: parent.left; right: parent.right; top: parent.top }
        videoInfo: viewModel.videoInfo
        overlaysVisible: viewModel.overlaysVisible
    }
```

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/TopBar.qml src/client/ui/overlay.qml && git commit -m "refactor: extract TopBar"
```

---

### Task 4: Extract CenterPlayBtn

**Files:**
- Create: `src/client/ui/CenterPlayBtn.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write CenterPlayBtn.qml**

```qml
import QtQuick
import QtQuick.Controls

Item {
    id: root
    property bool playing: false
    property bool overlaysVisible: true
    signal clicked()

    Rectangle {
        id: centerPlayBtn
        width: 72; height: 72; radius: 36
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        color: cpHover.hovered ? (cpMouse.pressed ? "#55000000" : "#44000000") : "#33000000"
        opacity: (!root.playing && root.overlaysVisible) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }

        Text {
            anchors.centerIn: parent
            font.family: iconFont; font.pixelSize: 36
            text: ""; color: "#ffffff"
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: cpMouse; anchors.fill: parent
            cursorShape: Qt.PointingHandCursor; hoverEnabled: true
            onClicked: root.clicked()
        }
        HoverHandler { id: cpHover }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Center Play Button` comment block + `Rectangle { id: centerPlayBtn ... }` (lines 97–119) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Center Play Button
    // ══════════════════════════════════════════════════════════════════

    CenterPlayBtn {
        playing: viewModel.playing
        overlaysVisible: viewModel.overlaysVisible
        onClicked: viewModel.togglePlayPause()
    }
```

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/CenterPlayBtn.qml src/client/ui/overlay.qml && git commit -m "refactor: extract CenterPlayBtn"
```

---

### Task 5: Extract ProgressSlider

**Files:**
- Create: `src/client/ui/ProgressSlider.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write ProgressSlider.qml**

The `Slider` originally references: `bottomBar.top` (anchor), `viewModel.duration`, `viewModel.currentTime`, `viewModel.overlaysVisible` (properties), `viewModel.seekAbsolute(value)` (call). All become root properties/signals. The `Connections { target: viewModel }` sync block becomes `onCurrentTimeChanged`. The hot zone MouseArea stays in overlay.qml.

```qml
import QtQuick
import QtQuick.Controls

Item {
    id: root
    property real duration: 0
    property real currentTime: 0
    property bool overlaysVisible: true
    signal seeked(real ms)

    implicitHeight: 14

    Slider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: psHover.hovered || pressed ? 8 : 6
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
        from: 0; to: root.duration
        live: false
        Behavior on height { NumberAnimation { duration: 150 } }

        background: Rectangle {
            color: "#44ffffff"
            Rectangle {
                width: progressSlider.visualPosition * parent.width
                height: parent.height; color: "#e0e0e0"
            }
            Rectangle {
                visible: psHover.hovered && !progressSlider.pressed
                width: 2; height: parent.height
                x: progressSlider.hoverX; color: "#aaffffff"
            }
        }

        handle: Rectangle {
            implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
            visible: psHover.hovered || progressSlider.pressed
            x: progressSlider.leftPadding + progressSlider.visualPosition * (progressSlider.availableWidth - width)
            y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
        }

        HoverHandler { id: psHover }
        property real hoverX: 0
        HoverHandler {
            onPointChanged: { if (hovered) progressSlider.hoverX = Math.min(Math.max(point.position.x, 0), progressSlider.availableWidth) }
        }

        onPressedChanged: {
            if (!pressed)
                root.seeked(value)
        }
    }

    onCurrentTimeChanged: {
        if (!progressSlider.pressed)
            progressSlider.value = root.currentTime
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace:
1. The `Slider { id: progressSlider ... }` block (original lines 154–201)  
2. The `Connections { target: viewModel ... }` block (original lines 207–213)

With:

```qml
    ProgressSlider {
        id: progressSlider
        anchors { left: parent.left; right: parent.right; bottom: bottomBar.top }
        duration: viewModel.duration
        currentTime: viewModel.currentTime
        overlaysVisible: viewModel.overlaysVisible
        onSeeked: viewModel.seekAbsolute(ms)
    }
```

Keep `progressHotZone` MouseArea (original lines 147–152) in overlay.qml unchanged — it references `bottomBar.top` which still exists.

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/ProgressSlider.qml src/client/ui/overlay.qml && git commit -m "refactor: extract ProgressSlider"
```

---

### Task 6: Extract BottomBar

**Files:**
- Create: `src/client/ui/BottomBar.qml`
- Modify: `src/client/ui/overlay.qml`

This is the most complex extraction. The BottomBar block contains IconButton instances that reference `viewModel.*` methods and `playlist.*` methods. All become signals. The `volBtn`/`qualBtn`/`spdBtn` ids are used by popups for positioning — replaced by `readonly property` centerX values.

Key mappings (external ref → component interface):
- `viewModel.playing` → `property bool playing`
- `viewModel.fullscreen` → `property bool fullscreen`
- `viewModel.hwDecoding` → `property bool hwDecoding`
- `viewModel.currentTime` / `viewModel.duration` → `property real currentTime / duration`
- `viewModel.overlaysVisible` → `property bool overlaysVisible` (for bar opacity)
- `viewModel.togglePlayPause()` → `signal playPauseClicked()`
- `viewModel.stop()` → `signal stopClicked()`
- `playlist.previous()` / `playlist.next()` → `signal prevClicked()` / `signal nextClicked()`
- `viewModel.loadFile(f)` → `signal fileSelected(string path)`
- `viewModel.toggleHwaccel()` → `signal hwaccelClicked()`
- `viewModel.toggleFullscreen()` → `signal fullscreenClicked()`
- `volumePopup.visible` → `property bool volumePopupOpen` (for btn highlighted)
- `qualityPopup.visible` → `property bool qualityPopupOpen`
- `speedPopup.visible` → `property bool speedPopupOpen`
- `playlistPanel.visible` → `property bool playlistOpen`
- `root.togglePlaylist()` → `signal playlistClicked()`
- `volBtn` id → `readonly property real volumeBtnCenterX`
- `qualBtn` id → `readonly property real qualityBtnCenterX`
- `spdBtn` id → `readonly property real speedBtnCenterX`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write BottomBar.qml**

```qml
import QtQuick
import QtQuick.Controls

Item {
    id: root
    property bool playing: false
    property bool fullscreen: false
    property bool hwDecoding: false
    property real currentTime: 0
    property real duration: 0
    property bool overlaysVisible: true
    property bool volumePopupOpen: false
    property bool qualityPopupOpen: false
    property bool speedPopupOpen: false
    property bool playlistOpen: false

    signal playPauseClicked()
    signal prevClicked()
    signal nextClicked()
    signal stopClicked()
    signal volumeClicked()
    signal qualityClicked()
    signal hwaccelClicked()
    signal speedClicked()
    signal fullscreenClicked()
    signal playlistClicked()

    implicitHeight: 48

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 48
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: "#cc000000" }
        }
        opacity: root.overlaysVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        HoverHandler { id: barHover }

        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 12 }
            spacing: 4

            IconButton { codepoint: ""; size: 22; tooltip: "上一个 (B)"
                onClicked: root.prevClicked() }
            IconButton { codepoint: root.playing ? "" : ""; size: 22
                tooltip: root.playing ? "暂停 (Space)" : "播放 (Space)"
                onClicked: root.playPauseClicked() }
            IconButton { codepoint: ""; size: 22; tooltip: "下一个 (N)"
                onClicked: root.nextClicked() }
            IconButton { codepoint: ""; size: 22; tooltip: "停止"
                onClicked: root.stopClicked() }

            Rectangle { width: 1; height: 20; color: "#0fffffff"; anchors.verticalCenter: parent.verticalCenter }

            Text {
                function fmt(ms) {
                    if (ms <= 0) return "0:00"
                    var s = Math.floor(ms/1000), m = Math.floor(s/60)
                    return m + ":" + (s%60 < 10 ? "0" : "") + s%60
                }
                text: fmt(root.currentTime) + " / " + fmt(root.duration)
                color: "#e0e0e0"; font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 12 }
            spacing: 4

            IconButton { id: volBtn; codepoint: ""; size: 22; tooltip: "音量"
                highlighted: root.volumePopupOpen
                onClicked: root.volumeClicked() }
            IconButton { id: qualBtn; codepoint: ""; size: 22; tooltip: "画质"
                highlighted: root.qualityPopupOpen
                onClicked: root.qualityClicked() }
            IconButton { label: root.hwDecoding ? "硬解" : "软解"; size: 22
                tooltip: root.hwDecoding ? "点击切换软解" : "点击切换硬解"
                onClicked: root.hwaccelClicked() }
            IconButton { id: spdBtn; label: "倍速"; size: 22; tooltip: "播放速度"
                highlighted: root.speedPopupOpen
                onClicked: root.speedClicked() }
            IconButton { codepoint: root.fullscreen ? "" : ""; size: 22
                tooltip: root.fullscreen ? "退出全屏" : "全屏"
                onClicked: root.fullscreenClicked() }
            IconButton { id: playlistBtn; codepoint: ""; size: 22; tooltip: "播放列表 (P)"
                highlighted: root.playlistOpen
                onClicked: root.playlistClicked() }
        }
    }

    // Center-X helpers for popup positioning (read from internal btn ids)
    readonly property real volumeBtnCenterX: {
        var p = volBtn.mapToItem(root.parent, 0, 0)
        return p.x + volBtn.width / 2
    }
    readonly property real qualityBtnCenterX: {
        var p = qualBtn.mapToItem(root.parent, 0, 0)
        return p.x + qualBtn.width / 2
    }
    readonly property real speedBtnCenterX: {
        var p = spdBtn.mapToItem(root.parent, 0, 0)
        return p.x + spdBtn.width / 2
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Bottom Bar` comment block + `Rectangle { id: bottomBar ... }` block (lines 216–284) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Bottom Bar
    // ══════════════════════════════════════════════════════════════════

    BottomBar {
        id: bottomBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        playing: viewModel.playing
        fullscreen: viewModel.fullscreen
        hwDecoding: viewModel.hwDecoding
        currentTime: viewModel.currentTime
        duration: viewModel.duration
        overlaysVisible: viewModel.overlaysVisible
        volumePopupOpen: volumePopup.visible
        qualityPopupOpen: qualityPopup.visible
        speedPopupOpen: speedPopup.visible
        playlistOpen: playlistPanel.visible
        onPlayPauseClicked: viewModel.togglePlayPause()
        onPrevClicked: { var f = playlist.previous(); if (f) viewModel.loadFile(f) }
        onNextClicked: { var f = playlist.next(); if (f) viewModel.loadFile(f) }
        onStopClicked: viewModel.stop()
        onVolumeClicked: volumePopup.visible ? volumePopup.close() : volumePopup.open()
        onQualityClicked: qualityPopup.visible ? qualityPopup.close() : qualityPopup.open()
        onHwaccelClicked: viewModel.toggleHwaccel()
        onSpeedClicked: speedPopup.visible ? speedPopup.close() : speedPopup.open()
        onFullscreenClicked: viewModel.toggleFullscreen()
        onPlaylistClicked: root.togglePlaylist()
    }
```

Note: `volumePopup.visible` / `qualityPopup.visible` / `speedPopup.visible` / `playlistPanel.visible` reference ids that still exist in overlay.qml (not yet extracted).

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/BottomBar.qml src/client/ui/overlay.qml && git commit -m "refactor: extract BottomBar"
```

---

### Task 7: Extract VolumePopup

**Files:**
- Create: `src/client/ui/VolumePopup.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write VolumePopup.qml**

```qml
import QtQuick
import QtQuick.Controls

Popup {
    id: root
    property real volume: 0.0
    property bool muted: false
    signal volumeChanged(real v)
    signal muteToggled()
    signal dismissed()

    width: 48; padding: 12
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: root.dismissed()

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column {
        spacing: 8; anchors.horizontalCenter: parent.horizontalCenter

        Slider {
            id: volSlider
            anchors.horizontalCenter: parent.horizontalCenter
            orientation: Qt.Vertical; height: 120
            from: 0; to: 100; live: true
            onMoved: root.volumeChanged(value / 100.0)
            handle: Rectangle { implicitWidth: 14; implicitHeight: 14; radius: 7; color: "#ffffff"
                x: parent.leftPadding + parent.availableWidth / 2 - width / 2
                y: parent.topPadding + parent.visualPosition * (parent.availableHeight - height) }
            background: Rectangle { implicitWidth: 4; color: "#44ffffff"; radius: 2
                anchors.horizontalCenter: parent.horizontalCenter
                Rectangle { width: 4; radius: 2; color: "#e0e0e0"
                    anchors.bottom: parent.bottom
                    height: parent.height * volSlider.position } }
        }

        Connections {
            target: root
            function onOpened() { volSlider.value = root.volume * 100 }
        }

        IconButton {
            anchors.horizontalCenter: parent.horizontalCenter
            codepoint: root.muted ? "" : ""; size: 20
            tooltip: root.muted ? "取消静音" : "静音"
            onClicked: { root.muteToggled(); volSlider.value = root.volume * 100 }
        }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Volume Popup` comment block + `Popup { id: volumePopup ... }` block (original lines 286–331) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Volume Popup
    // ══════════════════════════════════════════════════════════════════

    VolumePopup {
        id: volumePopup
        x: Math.min(Math.max(bottomBar.volumeBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        volume: viewModel.volume
        muted: viewModel.muted
        onVolumeChanged: viewModel.setVolume(v)
        onMuteToggled: viewModel.toggleMute()
        onDismissed: volumePopup.close()
    }
```

Note: `bottomBar.volumeBtnCenterX` and `bottomBar.y` are available after Task 6.

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/VolumePopup.qml src/client/ui/overlay.qml && git commit -m "refactor: extract VolumePopup"
```

---

### Task 8: Extract QualityPopup

**Files:**
- Create: `src/client/ui/QualityPopup.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write QualityPopup.qml**

The model arrays (`qualityScaleModel`, `qualityLevelModel`, `qualityDenoiseModel`) move from overlay root into QualityPopup — they're only used here.

```qml
import QtQuick
import QtQuick.Controls

Popup {
    id: root
    property int scal: 0
    property int qualit: 3
    property int denoiseQualit: -1
    signal scaleChanged(int v)
    signal qualityChanged(int v)
    signal denoiseQualityChanged(int v)
    signal dismissed()

    width: 360; padding: 16
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: root.dismissed()

    readonly property var scaleModel: [
        {label: "关闭", value: -1},
        {label: "自动", value: 0},
        {label: "2×", value: 2},
        {label: "3×", value: 3},
        {label: "4×", value: 4}
    ]
    readonly property var levelModel: [
        {label: "Low", value: 1},
        {label: "Medium", value: 2},
        {label: "High", value: 3},
        {label: "Ultra", value: 4}
    ]
    readonly property var denoiseModel: [
        {label: "关闭", value: -1},
        {label: "Low", value: 8},
        {label: "Medium", value: 9},
        {label: "High", value: 10},
        {label: "Ultra", value: 11}
    ]

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column {
        spacing: 12; anchors { left: parent.left; right: parent.right }

        Text { text: "Scale"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.scaleModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: sHover.containsMouse ? "#33ffffff"
                         : (root.scal === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.scal === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: sHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.scaleChanged(modelData.value); root.close() } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: "超分质量"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.levelModel
                delegate: Rectangle {
                    width: 70; height: 32; radius: 4
                    color: qHover.containsMouse ? "#33ffffff"
                         : (root.qualit === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.qualit === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.qualityChanged(modelData.value); root.close() } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: "强制降噪（scale=1时生效）"; color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.denoiseModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: dHover.containsMouse ? "#33ffffff"
                         : (root.denoiseQualit === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.denoiseQualit === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: dHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.denoiseQualityChanged(modelData.value); root.close() } }
                }
            }
        }
    }
}
```

Note: Properties are named `scal`/`qualit`/`denoiseQualit` to avoid shadowing `scale`/`quality` (reserved or ambiguous). The `qualityPopup.close()` calls in original delegates become `root.close()`.

- [ ] **Step 2: Update overlay.qml**

Replace:
1. The `═══ Quality Popup` comment block + `Popup { id: qualityPopup ... }` block (original lines 333–436)
2. The model arrays `qualityScaleModel`, `qualityLevelModel`, `qualityDenoiseModel` from overlay root (original lines 338–357)

With:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Quality Popup
    // ══════════════════════════════════════════════════════════════════

    QualityPopup {
        id: qualityPopup
        x: Math.min(Math.max(bottomBar.qualityBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        scal: viewModel.scale
        qualit: viewModel.quality
        denoiseQualit: viewModel.denoiseQuality
        onScaleChanged: viewModel.setScale(v)
        onQualityChanged: viewModel.setQuality(v)
        onDenoiseQualityChanged: viewModel.setDenoiseQuality(v)
        onDismissed: qualityPopup.close()
    }
```

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/QualityPopup.qml src/client/ui/overlay.qml && git commit -m "refactor: extract QualityPopup"
```

---

### Task 9: Extract SpeedPopup

**Files:**
- Create: `src/client/ui/SpeedPopup.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write SpeedPopup.qml**

```qml
import QtQuick
import QtQuick.Controls

Popup {
    id: root
    property real speed: 1.0
    signal speedChanged(real v)
    signal dismissed()

    width: 180; padding: 12
    modal: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: root.dismissed()

    background: Rectangle { color: "#d9111111"; radius: 8; border { width: 1; color: "#22ffffff" } }

    Column { spacing: 4; anchors { left: parent.left; right: parent.right }
        Text { text: "速度"; color: "#b0b0b0"; font.pixelSize: 13 }
        Repeater {
            model: [0.5, 0.75, 1.0, 2.0]
            Rectangle { width: parent.width; height: 36; radius: 4
                color: spm.containsMouse ? "#33ffffff" : "transparent"
                Text { anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    text: Number(modelData) + "倍"
                    color: Math.abs(root.speed - modelData) < 0.01 ? "#ffcc00" : "#e0e0e0"; font.pixelSize: 14 }
                MouseArea { id: spm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: { root.speedChanged(modelData); root.close() } }
            }
        }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Speed Popup` comment block + `Popup { id: speedPopup ... }` block (original lines 438–466) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Speed Popup
    // ══════════════════════════════════════════════════════════════════

    SpeedPopup {
        id: speedPopup
        x: Math.min(Math.max(bottomBar.speedBtnCenterX - width/2, 8), root.width - width - 8)
        y: bottomBar.y - height - 8
        speed: viewModel.speed
        onSpeedChanged: viewModel.setSpeed(v)
        onDismissed: speedPopup.close()
    }
```

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/SpeedPopup.qml src/client/ui/overlay.qml && git commit -m "refactor: extract SpeedPopup"
```

---

### Task 10: Extract PlaylistPanel

**Files:**
- Create: `src/client/ui/PlaylistPanel.qml`
- Modify: `src/client/ui/overlay.qml`

- [ ] **Step 0: Load qt-qml skill** — invoke `qt-development-skills:qt-qml` before writing QML code.

- [ ] **Step 1: Write PlaylistPanel.qml**

The Drawer references `playlist` context property (available via scope chain), `iconFont` (scope chain), and `root.togglePlaylist()`/`playlistPanel.close()` for close actions. The close button and file selection become signals.

```qml
import QtQuick
import QtQuick.Controls

Drawer {
    id: root
    edge: Qt.RightEdge
    width: 320; height: parent ? parent.height : 600; z: 10
    dragMargin: 0
    topPadding: 0; bottomPadding: 0; leftPadding: 0; rightPadding: 0
    background: Rectangle { color: "#d9000000" }

    signal fileSelected(string path)
    signal dismissed()

    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48; color: "#22ffffff"
        Text { anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            text: "播放列表"; color: "#e0e0e0"; font.pixelSize: 15; font.bold: true }
        Text { anchors { right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            text: playlist ? (playlist.currentIndex + 1) + "/" + playlist.count : ""
            color: "#b0b0b0"; font.pixelSize: 12 }

        Item { id: closeBtn; width: 34; height: 34
            anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
            Rectangle { anchors.fill: parent; radius: 4
                color: clHover.hovered ? "#33ffffff" : "transparent"
                Behavior on color { ColorAnimation { duration: 150 } } }
            Text { anchors.centerIn: parent; font.family: iconFont; font.pixelSize: 18
                text: ""; color: clHover.hovered ? "#ffffff" : "#c8c8c8"
                Behavior on color { ColorAnimation { duration: 150 } }
                renderType: Text.NativeRendering }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                onClicked: root.dismissed() }
            HoverHandler { id: clHover }
        }
    }

    ListView {
        id: playlistView
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
                  bottom: parent.bottom }
        model: playlist ? playlist.files : []
        cacheBuffer: 800
        clip: true

        delegate: Rectangle {
            id: plDelegate
            width: ListView.view.width; height: 42; clip: true
            color: plHover.hovered ? "#22ffffff"
                 : (index === playlist.currentIndex ? "#11ffffff" : "transparent")

            Row {
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }

                Text {
                    text: playlist && index < playlist.displayNames.length
                          ? (index + 1) + ". " + playlist.displayNames[index] : ""
                    width: 296
                    color: index === playlist.currentIndex ? "#ffffff" : "#b0b0b0"
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
            }

            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                onClicked: { playlist.setCurrentFile(modelData); root.fileSelected(modelData) }
            }

            HoverHandler { id: plHover }

            ToolTip {
                z: 11
                visible: plHover.hovered
                text: modelData
                delay: 600
                background: Rectangle {
                    color: "#d9111111"; radius: 4
                    border { width: 1; color: "#22ffffff" }
                }
                contentItem: Text {
                    text: modelData
                    color: "#e0e0e0"
                    font.pixelSize: 11
                }
            }
        }
    }
}
```

- [ ] **Step 2: Update overlay.qml**

Replace the `═══ Playlist Panel` comment block + `Drawer { id: playlistPanel ... }` block (original lines 468–559) and the dismiss overlay `MouseArea` (original lines 558–559) with:

```qml
    // ══════════════════════════════════════════════════════════════════
    // Playlist Panel
    // ══════════════════════════════════════════════════════════════════

    PlaylistPanel {
        id: playlistPanel
        onFileSelected: viewModel.loadFile(path)
        onDismissed: playlistPanel.close()
    }

    // Dismiss playlist (click outside)
    MouseArea { anchors { left: parent.left; right: parent.right; top: parent.top; bottom: bottomBar.top }
        visible: playlistPanel.visible; z: 5; onClicked: playlistPanel.close() }
```

Note: `root.togglePlaylist()` function in overlay.qml becomes:

```qml
    function togglePlaylist() { playlistPanel.visible ? playlistPanel.close() : playlistPanel.open() }
```

Update the existing `togglePlaylist()` function body (original line 576) from `playlistPanel.visible ? playlistPanel.close() : playlistPanel.open()` — unchanged functionally, just ensure the existing function still works.

- [ ] **Step 3: Build, verify, commit**

```bash
make -j$(nproc) && git add src/client/ui/PlaylistPanel.qml src/client/ui/overlay.qml && git commit -m "refactor: extract PlaylistPanel"
```

---

### Final Verification

After all 10 tasks, `overlay.qml` should contain only: imports, FontLoader, auto-hide logic, component instantiations with bindings, progress hot zone, fullscreen sync, keyboard shortcuts, and `togglePlaylist()` function. ~150 lines total.

- [ ] **Step 1: Verify overlay.qml still references correct ids**

The `hideTimer` `onTriggered` handler originally checked `volumePopup.visible || qualityPopup.visible || speedPopup.visible` — these ids still exist after Tasks 7-9. Verify the auto-hide expression is intact:

```qml
var popupOpen = volumePopup.visible || qualityPopup.visible || speedPopup.visible
```

- [ ] **Step 2: Full build**

```bash
make -j$(nproc)
```

- [ ] **Step 3: Manual playback test**

Run `./build/vsr-player <video_file>` and verify:
- Play/pause button (center + bottom bar) — toggle, icon changes
- Prev/next — file switching
- Stop button
- Progress slider — seek on release, hover preview, handle visibility
- Auto-hide — mouse move shows overlays, 3s timeout hides them
- Volume popup — open, close, slider changes volume, mute toggle
- Quality popup — scale/quality/denoise selection
- Speed popup — speed selection
- Hwaccel toggle — label switches between 硬解/软解
- Fullscreen toggle — icon changes, window mode toggles
- Playlist panel — open/close, file selection, header count
- OSD overlay — Tab key toggle, info display
- Keyboard shortcuts — Space (play/pause), P (playlist), F (fullscreen), Esc (stop/close)
- All popups dismiss on click-outside and Escape

- [ ] **Step 4: Final commit**

```bash
git add src/client/ui/overlay.qml
git commit -m "refactor: overlay.qml reduced to wiring layer after component extraction"
```
