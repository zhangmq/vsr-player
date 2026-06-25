# Overlay UI & Playlist — Design Spec (Qt Quick)

**Date:** 2026-06-23
**Status:** Approved
**Branch:** feature/overlay-ui

## Overview

Replace the Qt Widgets client with a Qt Quick overlay-based player interface inspired by YouTube/B站 fullscreen mode. All controls are overlaid on the video surface with auto-hide behavior. Add a playlist panel that slides in from the right, populated by folder traversal.

**Qt Quick was selected because:** on Wayland, Qt Widgets creates a `wl_subsurface` for Vulkan rendering that permanently occludes sibling QWidget overlays. Qt Quick renders everything on a single `wl_surface` — Vulkan video and QML overlays share the same render pass, eliminating the z-order problem entirely.

## Architecture

```
┌─────────────────────────────────────────────┐
│ QQuickView (single wl_surface)               │
│                                              │
│ beforeRenderPassRecording:                    │
│   ├── vkCmdClearAttachments (dark bg)         │
│   ├── PlayerCore::record_frame() (video quad) │
│   └── (render pass continues)                │
│                                              │
│ Qt Quick scene graph:                        │
│   ├── TopBar.qml                              │
│   ├── CenterPlayButton.qml                    │
│   ├── ControlBar.qml                          │
│   ├── SettingsMenu.qml (popup)                │
│   └── PlaylistPanel.qml (slide-in)            │
│                                              │
│ vkQueuePresentKHR (Qt handles)               │
└─────────────────────────────────────────────┘
```

**Renderer integration:** PlayerCore uses `initialize_external()` with Qt's VkDevice. The video draw is injected via `beforeRenderPassRecording` → `record_frame()` into Qt's active command buffer. The swapchain and presentation are managed entirely by Qt Quick's QRhi.

## UI Layers (bottom-to-top)

```
┌──────────────────────────────────────────┐
│  QML Scene                                │
│  ┌──────────────────────────────────────┐ │
│  │ Top Bar (48px)                        │ │
│  │   filename                            │ │
│  └──────────────────────────────────────┘ │
│  ┌──────────────────────────────────────┐ │
│  │ Center Play Button (72×72, paused)    │ │
│  └──────────────────────────────────────┘ │
│  ┌──────────────────────────────────────┐ │
│  │ Bottom Control Bar (56px)            │ │
│  │ ◀ ▶ ⏸ ■ ──●── 00:00/00:00 🔊 Q NV ⚙ ☰│ │
│  └──────────────────────────────────────┘ │
│  ┌────────────────────────────┐           │
│  │ Playlist Panel (320px)     │ ← slide   │
│  │ semi-transparent black     │           │
│  └────────────────────────────┘           │
└──────────────────────────────────────────┘
```

## Visibility Behavior

| Element | Trigger | Hide |
|---------|---------|------|
| Top Bar | Mouse movement / cursor near top edge | Fade out after 3s idle |
| Bottom Bar | Mouse movement / cursor near bottom edge | Fade out after 3s idle |
| Center Play Button | Playback paused AND overlays visible | When playing or overlays hidden |
| Playlist Panel | ☰ button click or keyboard shortcut | ✕ button, click outside, or toggle |

## Color Palette

| Role | Value |
|------|-------|
| Video background | `#0a0a0a` |
| Overlay gradient (top) | `rgba(0,0,0,0.8)` → `transparent` |
| Overlay gradient (bottom) | `transparent` → `rgba(0,0,0,0.8)` |
| Icon default | `#c8c8c8` |
| Icon hover | `#ffffff` |
| Text primary | `#e0e0e0` |
| Text secondary | `#b0b0b0` |
| Panel background | `rgba(0,0,0,0.85)` + `backdrop-filter: blur(16px)` |
| Active item highlight | `rgba(255,255,255,0.06)` |
| Separator | `rgba(255,255,255,0.06)` |

## Components

### 1. Top Bar (QML `Rectangle`)

- **Position:** Top of window, 48px height
- **Content:** Current filename, left-aligned `Text`
- **Visibility:** Declarative `Behavior on opacity` driven by `MouseArea` → `Timer`
- **No buttons** — settings and playlist toggles live in the bottom bar only

### 2. Bottom Control Bar (QML `Rectangle` + `Row`)

- **Position:** Bottom of window, 56px height
- **Content (left to right in a `Row` layout):**

| # | Control | Icon | Size | Tooltip | Player API |
|---|---------|------|------|---------|------------|
| 1 | Previous | ◀ skip-back (solid) | 22×22 | "上一个" | `LOAD_FILE` (previous in playlist) |
| 2 | Play/Pause | ▶ / ⏸ (solid) | 28×28 | "播放/暂停 (Space)" | `PLAY` / `PAUSE` |
| 3 | Next | ▶ skip-forward (solid) | 22×22 | "下一个" | `LOAD_FILE` (next in playlist) |
| 4 | Stop | ■ square (solid) | 20×20 | "停止" | `STOP` |
| 5 | Progress Bar | `Slider` with custom style | flex | — | `SEEK` |
| 6 | Time | `MM:SS / MM:SS` | text 13px | — | — |
| 7 | Volume | 🔊 speaker (solid body + 1 arc) | 22×22 | "音量" | `SET_VOLUME` |
| 8 | Quality | Text label: "HIGH" | 13px + badge | "画质/VSR" | `SET_QUALITY` + VSR toggle |
| 9 | HW/SW | Text label: "NVDEC" / "SW" | 13px + badge | "硬解/软解" | Toggle hwaccel (rebuild pipeline) |
| 10 | Settings | ⚙ gear (solid) | 20×20 | "设置" | Opens `SettingsMenu` |
| 11 | Playlist | ☰ list (solid dots + lines) | 20×20 | "播放列表" | Toggles `PlaylistPanel` |

**IconButton component:** Reusable QML `Item` with `Shape` + `PathSvg` (GPU-accelerated, declarative) — takes `iconPath` (SVG path string), `iconSize`, and `iconColor` / `hoverColor` / `pressedColor` properties. Each button is instantiated declaratively, e.g.:
```qml
IconButton { icon: "play"; size: 28; onClicked: controller.play() }
```

### 3. Settings Menu (QML `Popup`)

- Anchored above the settings button
- **Items:**
  - VSR: OFF / LOW / MEDIUM / HIGH / ULTRA (radio group)
  - Scale: 1× / 1.5× / 2× / 3× / 4× (if VSR enabled)
  - Separator
  - Screenshot (CAPTURE_FRAME)
- Dismisses on click outside or selection

### 4. Playlist Panel (QML `Rectangle` + `ListView`)

- **Position:** Right side, 320px width, full height
- **Animation:** Slide in/out from right (`NumberAnimation` on `x`, ~200ms)
- **Background:** `#D9000000` (85% black) with `FastBlur` effect
- **Header:** "播放列表" title + "N files" info + ✕ close button
- **List:** `ListView` with custom `delegate`
  - Active item: `rgba(255,255,255,0.06)` background
  - Inactive items: `#999` text
  - Click to load file via `LOAD_FILE`
- **Close:** ✕ button, ☰ toggle, click outside, or Escape key

### 5. Center Play Button (QML `Rectangle` + `Shape`)

- Visible only when paused AND overlays are shown (see Animation section)
- 72×72 circle, `#66000000`, centered on video, `Behavior on color` for hover
- Play/pause icon via `Shape` + `PathSvg` (see IconButton)
- Click → `PLAY`

### 6. Playlist Engine (`PlaylistEngine` — C++, no Qt dependency beyond `QString`)

- **Input:** Root directory path + traversal depth
- **Traversal:** BFS up to `--depth` (default 3). Sorted by filename, flattened.
- **Filter:** Common video extensions (`.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.ts`, `.m2ts`, `.flv`, `.wmv`). FFmpeg demuxer validates on open, skip on failure.
- **CLI:** Smart path detection — if argument is a directory → playlist mode; if file → single video mode.
- **API:** `scan_folder(path, depth)` → `QStringList`, `current_index()`, `next()`, `previous()`
- **Exposed to QML** via `rootContext()->setContextProperty("playlist", &engine)`

## Icon System

**Approach:** Qt Quick `Shape` + `PathSvg` — GPU-accelerated, declarative SVG path rendering. No `Canvas`, no imperative JS.

```qml
// IconPaths.qml — singleton source of truth for all icon SVG path strings
pragma Singleton
import QtQuick

QtObject {
    readonly property string play: "M8 5v14l11-7z"
    readonly property string pause: "M6 19h4V5H6v14zm8-14v14h4V5h-4z"
    readonly property string skipBack: "M18 6L10 12l8 6V6zM6 6v12h2V6H6z"
    readonly property string skipForward: "M6 18l8-6-8-6v12zM18 6v12h2V6h-2z"
    readonly property string stop: "M6 6h12v12H6z"
    readonly property string volume: "M11 5L6 9H2v6h4l5 4V5z"
    readonly property string volumeMuted: "M11 5L6 9H2v6h4l5 4V5zM23 9l-6 6m0-6l6 6"
    readonly property string settings: "M12 15a3 3 0 100-6 3 3 0 000 6z"
    readonly property string playlist: "M4 6h16M4 12h16M4 18h16"
    readonly property string close: "M18 6L6 18M6 6l12 12"
}
```

```qml
// IconButton.qml — reusable declarative button
Item {
    property string iconPath
    property real iconSize: 22
    property color iconColor: "#c8c8c8"
    property color hoverColor: "#ffffff"
    property color pressedColor: "#aaaaaa"
    signal clicked()

    property bool _hovered: false
    property bool _pressed: false

    Shape {
        anchors.centerIn: parent
        width: iconSize; height: iconSize
        layer.enabled: true
        layer.samples: 4  // antialiasing

        ShapePath {
            fillColor: _pressed ? pressedColor
                     : _hovered ? hoverColor : iconColor
            strokeColor: "transparent"
            PathSvg { path: iconPath }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        hoverEnabled: true
        onEntered: _hovered = true
        onExited: { _hovered = false; _pressed = false }
        onPressed: _pressed = true
        onReleased: { if (_pressed) clicked(); _pressed = false }
    }
}
```

Paths sourced from Feather Icons (MIT). All icons: **solid fill**.

## Animation & Interaction

**Principle:** Declarative Qt Quick, not imperative JS. Use `Behavior`, `Transition`, `Animator`, and `States` — never `Timer` + manual property assignment.

### Auto-hide

```qml
// overlay.qml root
MouseArea {
    anchors.fill: parent; hoverEnabled: true
    onPositionChanged: { showOverlays(); idleTimer.restart() }
}

Timer {
    id: idleTimer; interval: 3000
    onTriggered: overlaysVisible = false
}

property bool overlaysVisible: true

// Each overlay component binds opacity to this flag:
Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
opacity: overlaysVisible ? 1.0 : 0.0
```

### Center Play Button show/hide

Tied to playback state + overlay visibility:
```qml
opacity: (controller.playing || !overlaysVisible) ? 0.0 : 1.0
Behavior on opacity { NumberAnimation { duration: 200 } }
```

### Playlist slide-in

```qml
Rectangle {
    id: playlistPanel
    x: parent.width  // offscreen (right)
    NumberAnimation on x {
        id: slideAnim; duration: 200; easing.type: Easing.OutQuad
    }
    function toggle() {
        slideAnim.to = (x > parent.width/2) ? parent.width - 320 : parent.width
        slideAnim.start()
    }
}
```

### Button hover/press

Use `Behavior on color` + `ColorAnimation`, not explicit hover states:
```qml
color: "#66000000"
Behavior on color { ColorAnimation { duration: 100 } }
// MouseArea hover/pressed sets color declaratively via properties
```

### Progress bar scrub

```qml
Slider {
    from: 0; to: duration
    value: currentTime
    onMoved: controller.seek(value)
    // Custom style via `background` and `handle` items
}
```

## HiDPI / Pixel Coordinates

**Qt Quick handles two coordinate spaces, and they must not be confused:**

| Space | Unit | Source | Used by |
|-------|------|--------|---------|
| Logical | device-independent pixels | `view.size()`, QML `width`/`height`/`anchors` | QML layout, UI sizing |
| Physical | actual framebuffer pixels | `view.size() × devicePixelRatio()` | Vulkan viewport, scissor, clear rect, swapchain extent |

**Key rules:**

1. `QQuickWindow::size()` returns **logical** pixels. Multiply by `devicePixelRatio()` for Vulkan operations.
2. QML items (`Rectangle`, `Text`, etc.) all use **logical** pixels automatically — no conversion needed in QML.
3. `record_to_cb(cb, w, h)` receives **physical** pixels (`w = logicalW × DPR`, `h = logicalH × DPR`).
4. `VkClearRect` and `VkViewport` use **physical** pixels.
5. All QML mouse/touch coordinates are in **logical** pixels.

**Example (2× HiDPI, 800×600 logical window):**
```
view.size()               → (800, 600)   logical
view.devicePixelRatio()   → 2.0
vkCmdClearAttachments cb  → extent (1600, 1200)  physical
record_to_cb(cb, 1600, 1200)  // physical
QML Rectangle { width: 72 }   → 144 physical px  // Qt Quick scales automatically
```

**Common mistakes from Qt Widgets era (avoid these):**

- ❌ Using logical pixel values for Vulkan viewport → undersized rendering
- ❌ Using physical pixels in QML → UI 2× too large
- ❌ Not accounting for DPR change when window moves between monitors → stale swapchain

## CLI

```
vsr-player [--no-vsr] [--no-hwaccel] [--quality LOW|MEDIUM|HIGH|ULTRA]
           [--depth N] <path>
```

- `<path>`: If directory → playlist mode (scan folder). If file → single video mode.
- `--depth N`: Traversal depth for folder scan (default 3). Ignored for single file.
- Existing flags unchanged.

## File Change Matrix

| File | Action | Description |
|------|--------|-------------|
| `src/client/main.cpp` | **Rewrite** | QGuiApplication + QQuickView + PlayerCore Vulkan integration |
| `src/client/overlay.qml` | **New** | Root QML scene: TopBar + ControlBar + CenterPlayButton + PlaylistPanel + SettingsMenu |
| `src/client/IconButton.qml` | **New** | Reusable button (Shape + PathSvg + hover/pressed + Behavior on color) |
| `src/client/IconPaths.qml` | **New** | Singleton — all SVG icon path strings (Feather Icons MIT) |
| `src/client/PlaylistEngine.h/cpp` | **New** | Folder traversal, file filtering, playlist navigation |
| `src/client/MainWindow.h/cpp` | **Delete** | Replaced by QQuickView in main.cpp |
| `src/client/VulkanWidget.h/cpp` | **Delete** | Video rendering via beforeRenderPassRecording |
| `src/client/PlayPauseButton.h/cpp` | **Delete** | QML CenterPlayButton replaces |
| `Makefile` | **Modify** | `Qt6Widgets` → `Qt6Quick`, `src/client_quick` references removed, single `vsr-player` target |
| `src/client_quick/` | **Delete** | Merged into src/client/ |
| `src/core/PlayerCore.h/cpp` | **Keep** | Already supports external mode with PlayerCore::initialize_external() |
| `src/core/utils/VulkanRenderer.h/cpp` | **Keep** | Already has init_external() + record_to_cb() |
| `src/core/utils/VulkanContext.h/cpp` | **Keep** | Already has init_external() |
| `src/client/shaders/` | **Keep** | SPIR-V shaders unchanged |
| `third_party/` | **Keep** | Unchanged |

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Space | Play/Pause |
| Escape | Close playlist panel / stop |
| Left/Right | Seek ±5s / ±10s (hold Shift) |
| Up/Down | Volume ±5% |
| F | Toggle fullscreen |
| S | Screenshot |
| P | Toggle playlist panel |
| N | Next in playlist |
| B | Previous in playlist |

## Error Handling

- **Scan failures:** Non-readable directories → log warning, skip
- **Unplayable files:** FFmpeg demuxer open failure → skip silently, log
- **Empty directory:** "No playable files found" in playlist panel
- **HW/SW toggle failure:** Pipeline rebuild fails → revert to previous state, show error text

## Testing

| Test | Method |
|------|--------|
| Icon rendering | Visual inspection, all sizes and colors |
| Auto-hide behavior | Manual: move mouse → show; idle 3s → fade |
| Playlist scan | Unit test: `PlaylistEngine::scan_folder()` with test dir tree |
| Playlist navigation | Manual: previous/next cycle through list |
| Quality switch | Manual: change quality → verify VSR reconfig |
| HW/SW toggle | Manual: toggle → verify pipeline rebuild, no crash |
| CLI parsing | Integration: `--depth`, smart path detection |
| Window close with overlay | Verify no crash via timeout exit |
| Transparent overlay on video | Visual: QML button + bar visible against Vulkan video |
