# Playlist Performance — Implementation Plan

> **For agentic workers:** Use subagent-driven-development or inline execution. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Eliminate playlist lag with hundreds of entries by pre-computing display names in C++ and removing expensive text measurement from QML delegate.

**Architecture:** PlaylistEngine pre-computes basenames at scan time → `displayNames` QStringList exposed to QML. ListView delegate uses `displayNames[index]` with `clip: true` hard-crop (no Elide measurement) + `cacheBuffer`. ToolTip on hover for truncated names.

**Tech Stack:** Qt 6 QML, C++17

---

### Task 1: Add displayNames property to PlaylistEngine

**Files:**
- Modify: `src/client/PlaylistEngine.h:10-12`
- Modify: `src/client/PlaylistEngine.cpp:16-36`

- [ ] **Step 1: Add Q_PROPERTY declaration**

```cpp
// PlaylistEngine.h — add after "files" property
Q_PROPERTY(QStringList displayNames READ displayNames NOTIFY filesChanged)
```

And add the accessor declaration:

```cpp
QStringList displayNames() const { return displayNames_; }
```

And add the member variable:

```cpp
QStringList displayNames_;
```

- [ ] **Step 2: Populate displayNames in scanFolder**

In `scanFolder()`, after `files_.append(fi.absoluteFilePath())`:

```cpp
} else if (fi.isFile()) {
    files_.append(fi.absoluteFilePath());
    displayNames_.append(fi.fileName());  // <-- add this
}
```

- [ ] **Step 3: Populate displayNames in scanDir**

In `scanDir()`, after `files_.append(info.absoluteFilePath())`:

```cpp
if (kExtensions.contains(ext)) {
    files_.append(info.absoluteFilePath());
    displayNames_.append(info.fileName());  // <-- add this
}
```

- [ ] **Step 4: Clear displayNames on new scan**

In `scanFolder()`, alongside `files_.clear()`:

```cpp
files_.clear();
displayNames_.clear();  // <-- add this
```

- [ ] **Step 5: Commit**

```bash
git add src/client/PlaylistEngine.h src/client/PlaylistEngine.cpp
git commit -m "feat: add displayNames property to PlaylistEngine"
```

---

### Task 2: Simplify QML delegate for performance

**Files:**
- Modify: `src/client/ui/overlay.qml:473-488`

- [ ] **Step 1: Replace ListView delegate**

Replace lines 473-488:

```qml
ListView { anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
                     bottom: parent.bottom }
    model: playlist ? playlist.files : []; clip: true
    delegate: Rectangle { width: 320; height: 42
        color: plHover.hovered ? "#22ffffff"
             : (index === playlist.currentIndex ? "#11ffffff" : "transparent")
        Row { anchors.left: parent.left; anchors.leftMargin: 16; anchors.verticalCenter: parent.verticalCenter; spacing: 8
            Text { text: (index+1) + "."; width: 28; horizontalAlignment: Text.AlignRight
                color: index === playlist.currentIndex ? "#e0e0e0" : "#b0b0b0"; font.pixelSize: 13 }
            Text { text: modelData.split('/').pop(); width: 250; elide: Text.ElideRight
                color: index === playlist.currentIndex ? "#ffffff" : "#b0b0b0"; font.pixelSize: 13 } }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
            onClicked: { playlist.setCurrentFile(modelData); viewModel.loadFile(modelData) } }
        HoverHandler { id: plHover }
    }
}
```

With:

```qml
ListView {
    id: playlistView
    anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
              bottom: parent.bottom }
    model: playlist ? playlist.files : []
    cacheBuffer: height * 2
    clip: true

    delegate: Rectangle {
        width: ListView.view.width; height: 42; clip: true
        color: plHover.hovered ? "#22ffffff"
             : (index === playlist.currentIndex ? "#11ffffff" : "transparent")

        Row {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            spacing: 8

            Text {
                text: index + 1 + "."
                width: 28; horizontalAlignment: Text.AlignRight
                color: index === playlist.currentIndex ? "#e0e0e0" : "#b0b0b0"
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Text {
                text: playlist && index < playlist.displayNames.length
                      ? playlist.displayNames[index] : ""
                width: 250
                color: index === playlist.currentIndex ? "#ffffff" : "#b0b0b0"
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }

        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
            onClicked: { playlist.setCurrentFile(modelData); viewModel.loadFile(modelData) }
        }

        HoverHandler { id: plHover }

        ToolTip {
            visible: plHover.hovered
            text: modelData
            delay: 600
            font.pixelSize: 11
            background: Rectangle {
                color: "#d9111111"; radius: 4
                border { width: 1; color: "#22ffffff" }
            }
            contentItem: Text {
                text: modelData; color: "#e0e0e0"; font.pixelSize: 11
            }
        }
    }
}
```

- [ ] **Step 2: Build and verify compilation**

```bash
make -j$(nproc) 2>&1
```

- [ ] **Step 3: Commit**

```bash
git add src/client/ui/overlay.qml
git commit -m "perf: simplify playlist delegate — pre-computed names, clip crop, cacheBuffer"
```

---

## Verification

1. Build passes with `make -j$(nproc)`
2. Load folder with many video files, open playlist — no visible lag
3. Scroll rapidly — smooth, no stutter
4. Hover truncated name — ToolTip shows full path
5. Click item — loads file correctly
