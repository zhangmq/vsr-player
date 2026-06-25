# Playlist Performance — Design Spec

**Date:** 2026-06-26

**Goal:** Playlist panel with hundreds of entries scrolls smoothly, no lag on open or during scroll.

## Problem

Current `ListView` delegate does `Text.ElideRight` (expensive text measurement) and `modelData.split('/').pop()` (per-frame JS string alloc). With hundreds of items, these compound — especially during Drawer slide-in when delegates are created under animation frame budget pressure.

## Design

### 1. Pre-computed display names (C++ side)

`PlaylistEngine` exposes a new `QStringList displayNames` property populated at scan time — each entry is the basename (`QFileInfo(path).fileName()`). QML no longer calls `split('/').pop()` on every delegate binding.

```
// PlaylistEngine.h — new property
Q_PROPERTY(QStringList displayNames READ displayNames NOTIFY filesChanged)
```

Accessed by index in the delegate: `displayNames[index]`.

### 2. Simplified delegate — no text measurement

- Remove `Text.ElideRight`
- Set `clip: true` on delegate `Rectangle` or inner `Row` — content beyond the fixed width is hard-clipped (zero measurement cost)
- `ToolTip` on hover shows the full file path from `files[index]`
- `renderType: Text.NativeRendering` on delegate text (GPU raster, matches rest of UI)

### 3. Cache buffer

Increase `ListView.cacheBuffer` from default (0) to ~2× viewport height. This pre-creates delegates above and below the visible area so scroll doesn't hit instant-delegate-creation on every new row.

### 4. Delegate reuse / keying

No change needed — Qt Quick ListView with fixed-height delegates already recycles instances. But ensure delegate identity is stable by binding to `index`, not `modelData`, for state that varies per-row.

### Files changed

| File | Change |
|------|--------|
| `src/client/PlaylistEngine.h` | Add `displayNames` property |
| `src/client/PlaylistEngine.cpp` | Populate `displayNames` in `scanFolder`, clear in `scanDir` |
| `src/client/ui/overlay.qml` | Simplify delegate, session-state flag for first-open no-animation |

## QML Delegate — After

```qml
ListView {
    anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48; bottom: parent.bottom }
    model: playlist ? playlist.files : []
    displayMarginEnd: 0
    cacheBuffer: height * 2
    clip: true

    delegate: Rectangle {
        width: 320; height: 42; clip: true
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
                width: 250; elide: Text.ElideNone
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

## Deferred Animation on First Open

For the Drawer slide-in on first open: use a session-level QML property (`firstPlaylistOpen: true`). On first toggle, close the drawer immediately with `enter: null` Transition override, then restore the transition for subsequent toggles.

```qml
property bool firstPlaylistOpen: true

function togglePlaylist() {
    if (firstPlaylistOpen) {
        // Suppress enter animation on first open
        playlistPanel.enter = null
        playlistPanel.open()
        firstPlaylistOpen = false
        // Restore enter on next event loop tick so subsequent opens animate
        Qt.callLater(function() {
            playlistPanel.enter = playlistEnter
        })
    } else {
        playlistPanel.visible ? playlistPanel.close() : playlistPanel.open()
    }
}

Transition { id: playlistEnter; ... }
```

## Verification

1. Load a folder with 500+ video files, open playlist — no visible lag during slide-in
2. Scroll rapidly through the list — smooth 60fps, no jank
3. Hover over truncated filename — ToolTip shows full path after 600ms
4. Subsequent open/close — slide animation works normally
5. Click item — loads file, currentIndex highlight updates
