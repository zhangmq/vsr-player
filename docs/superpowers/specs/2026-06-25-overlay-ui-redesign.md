# Overlay UI Redesign — Design Spec

**Date:** 2026-06-25
**Status:** Draft
**Reference:** YouTube player UX model, aligned to product-grade

## 1. Architecture

```
┌───────────────────────────────────────────────────────┐
│ QQuickView (single wl_surface)                         │
│                                                        │
│ beforeRenderPassRecording:                             │
│   ├── vkCmdClearAttachments (#0a0a0a)                  │
│   └── PlayerCore::record_frame(cb, physW, physH)       │
│                                                        │
│ QML Scene Graph (overlay on same surface):              │
│   ├── ProgressSlider (bottom bar 顶部)                  │
│   ├── TopBar (48px, filename, no buttons)               │
│   ├── CenterPlayButton (72×72, paused only)             │
│   ├── BottomBar (48px, all controls)                   │
│   │   ├── IconButton × N (Material Icons font)         │
│   │   ├── VolumePopup (竖滑条)                          │
│   │   ├── QualityPopup (centered panel)                 │
│   │   └── SpeedPopup (centered panel)                   │
│   └── PlaylistPanel (320px, right slide-in)             │
└───────────────────────────────────────────────────────┘
```

## 2. Layout

```
┌───────────────────────────────────────────────────────┐
│ TopBar (48px, gradient #cc000000→transparent)         │
│  filename Text, left-aligned                           │
├───────────────────────────────────────────────────────┤
│                                                        │
│                    [CenterPlayButton]                   │
│                    72×72 circle, centered               │
│                    visible: paused + overlays           │
│                                                        │
├───────────────────────────────────────────────────────┤
│ ProgressSlider (4px, full-width, hover→6px)            │
│  borderless, track #44ffffff, filled #e0e0e0           │
│  handle: 14×14 circle, white, visible on hover/touch   │
├───────────────────────────────────────────────────────┤
│ BottomBar (48px, gradient transparent→#cc000000)       │
│                                                        │
│  Row (spacing: 4, LeftToRight):                        │
│   ◀  ▶  ⏯  ⏹  |  0:00/0:00  |  🔊  Q  SPD  ⛶  ☰    │
│                                                        │
│  Popups (per-control, anchored above their button):    │
│   VolumePopup — vertical slider, 160×36               │
│   QualityPopup — centered panel, 200×auto              │
│   SpeedPopup — centered panel, 160×auto                │
│   PlaylistPanel — right slide-in, 320×full-height      │
└───────────────────────────────────────────────────────┘
```

## 3. Color System

| Token | Value | Usage |
|-------|-------|-------|
| `colorBg` | `#0a0a0a` | Video clear color |
| `colorOverlayStart` | `#cc000000` | Gradient solid end |
| `colorOverlayEnd` | `transparent` | Gradient fade end |
| `colorTextPrimary` | `#e0e0e0` | Labels, titles, time |
| `colorTextSecondary` | `#b0b0b0` | Metadata, inactive items |
| `colorIconDefault` | `#c8c8c8` | Regular icon state |
| `colorIconHover` | `#ffffff` | Hovered icon state |
| `colorProgressTrack` | `#44ffffff` | Slider background |
| `colorProgressFill` | `#e0e0e0` | Slider played portion |
| `colorProgressHandle` | `#ffffff` | Slider thumb |
| `colorPanelBg` | `#d9111111` | Popup panel background |
| `colorPanelBorder` | `#22ffffff` | Popup subtle border |
| `colorItemHover` | `#33ffffff` | Menu item hover |
| `colorItemActive` | `#ffcc00` | Current selection highlight |
| `colorSeparator` | `rgba(255,255,255,0.06)` (`#0fffffff`) | Dividers |

## 4. Icon System — Material Icons

**Font:** Material Icons (Google, Apache 2.0). Bundled as `third_party/fonts/MaterialIcons-Regular.ttf`. Loaded via Qt `FontLoader` at application startup.

**Usage:** Every icon is a QML `Text` element with `font.family: materialIcons.name` and `text: <codepoint>` (unicode escape). Exact codepoints are looked up from the font's character map at implementation time — the icon *names* below map to the official Material Icons glyph set. No SVG paths, no Shape+PathSvg, no custom drawing.

| Control | Material Icon name | Size |
|---------|-------------------|------|
| Previous | skip_previous | 22 |
| Next | skip_next | 22 |
| Play | play_arrow | 28 |
| Pause | pause | 28 |
| Stop | stop | 20 |
| Volume (high) | volume_up | 22 |
| Volume (muted) | volume_off | 22 |
| Quality | high_quality | 22 |
| Speed | speed | 22 |
| Fullscreen | fullscreen | 20 |
| Fullscreen exit | fullscreen_exit | 20 |
| Playlist | playlist_play | 20 |
| Close | close | 18 |

**Codepoint lookup table** is generated once from the font file's `cmap` table and committed as a constant. Implementation: run `python3 -c "from fontTools.ttLib import TTFont; ..."` or equivalent to dump the name→unicode mapping.

**Font loading:**
```qml
FontLoader {
    id: materialIcons
    // Bundled with the application in third_party/fonts/
    source: "file:///usr/share/vsr-player/fonts/MaterialIcons-Regular.ttf"
}
```
The font file is installed alongside the binary (Makefile `install` target copies it). At development time, the absolute path to the repo is used.

## 5. Components

### 5.1 TopBar

- Height: 48px
- Background: vertical gradient `#cc000000` → `transparent`
- Content: single `Text`, left-aligned, `colorTextPrimary`, font 14px
- Text logic: `pendingFile ? "⟳ " + basename(pendingFile) : videoInfo`
- No buttons, no popup triggers
- Auto-hide: opacity bound to `overlaysVisible`

### 5.2 CenterPlayButton

- 72×72 circle, centered on video surface
- Background: `#66aa2222` (default), lighter on hover, darker on press
- Icon: `play_arrow` (28px), inverted from play/pause state
- Visible: `opacity: (!playing && overlaysVisible) ? 1.0 : 0.0`
- Click → `controller.togglePlayPause()`
- `Behavior on opacity { NumberAnimation { duration: 200 } }`
- `Behavior on color { ColorAnimation { duration: 100 } }`

### 5.3 ProgressSlider

- Position: anchored to bottom of parent, above BottomBar. 4px height.
- Hover: expands to 6px height (`Behavior on height`)
- Track: `colorProgressTrack`, full width
- Fill: `colorProgressFill`, width = `visualPosition * parent.width`
- Handle: 14×14 circle, `colorProgressHandle`, visible on `hovered || pressed`
- Value: bound to `controller.currentTime`
- Range: 0 → `controller.duration`
- `onMoved: controller.seekAbsolute(value)`

```qml
Slider {
    id: progressSlider
    anchors { left: parent.left; right: parent.right; bottom: bottomBar.top }
    height: progressHover.hovered ? 6 : 4
    from: 0
    to: controller ? controller.duration : 1
    value: controller ? controller.currentTime : 0
    live: false  // only seek on release

    Behavior on height { NumberAnimation { duration: 150 } }

    background: Rectangle {
        color: colorProgressTrack
        Rectangle {
            width: progressSlider.visualPosition * parent.width
            height: parent.height
            color: colorProgressFill
        }
    }

    handle: Rectangle {
        width: 14; height: 14; radius: 7
        color: colorProgressHandle
        visible: progressHover.hovered || progressSlider.pressed
    }

    HoverHandler { id: progressHover }

    onMoved: controller.seekAbsolute(value)
}
```

### 5.4 BottomBar

- Height: 48px
- Background: vertical gradient `transparent` → `#cc000000`
- Content: `Row` layout, 4px spacing, left-to-right

**Button order (L→R):**

```
Prev  Next  PlayPause  Stop  |  Time  |  Volume  Quality  Speed  Fullscreen  Playlist
```

- Separator `|` = `Rectangle { width: 1; height: 20; color: colorSeparator }`
- Time label: `Text { font: 13px; color: colorTextPrimary }`, format `M:SS / M:SS`
- Each button: `IconButton` with popup where applicable

### 5.5 IconButton (shared component)

```qml
// IconButton.qml
Item {
    id: root
    property string iconText       // Material Icons unicode char
    property real iconSize: 22
    property string tooltipText: ""
    property bool showPopup: false  // set externally for popup-having buttons
    signal clicked()

    implicitWidth: iconSize + 16
    implicitHeight: iconSize + 16

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: btnHover.hovered || root.showPopup ? "#22ffffff" : "transparent"
        Behavior on color { ColorAnimation { duration: 150 } }
    }

    Text {
        anchors.centerIn: parent
        font.family: materialIcons.name
        font.pixelSize: root.iconSize
        text: root.iconText
        color: (btnHover.hovered || root.showPopup) ? colorIconHover : colorIconDefault
        Behavior on color { ColorAnimation { duration: 150 } }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        hoverEnabled: true
        onClicked: root.clicked()
    }

    HoverHandler { id: btnHover }

    // Tooltip
    Text {
        visible: btnHover.hovered && tooltipText !== ""
        text: tooltipText
        color: colorTextPrimary
        font.pixelSize: 11
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.bottom; topMargin: 2 }
    }
}
```

### 5.6 VolumePopup

- Trigger: hover on Volume icon → show; leave icon AND popup → hide (300ms delay)
- Both the icon and the popup have `HoverHandler` instances. A single shared state `volumePopupVisible` is true when either is hovered. The hide timer starts only when both lose hover.
- Position: anchored above volume button, horizontally centered
- Size: 36×160px
- Content: vertical `Slider` + Text showing current value (e.g. "65")
- Range: 0–100
- `onMoved: controller.setVolume(value / 100.0)`
- Background: `colorPanelBg`, radius 6px

```qml
Popup {
    id: volumePopup
    x: volumeBtn.x + (volumeBtn.width - width) / 2
    y: volumeBtn.y - height - 8
    width: 36; height: 160
    padding: 8

    background: Rectangle {
        color: colorPanelBg; radius: 6
        border { width: 1; color: colorPanelBorder }
    }

    Slider {
        anchors.centerIn: parent
        orientation: Qt.Vertical
        from: 100; to: 0  // inverted: top is max
        value: controller ? controller.volume * 100 : 65
        onMoved: controller.setVolume(value / 100.0)

        handle: Rectangle { width: 14; height: 14; radius: 7; color: colorIconHover }
        background: Rectangle {
            implicitWidth: 4; implicitHeight: 130
            color: colorProgressTrack
            Rectangle {
                width: 4
                height: parent.height * (1 - volumeSlider.visualPosition)
                color: colorProgressFill
            }
        }
    }
}
```

### 5.7 QualityPopup

- Trigger: click Quality button → toggle
- Position: centered on video (horizontal center, vertical center)
- Size: 280×auto
- Background: `colorPanelBg`, radius 8px
- Content:
  - Title: "Quality" (13px, `colorTextSecondary`, top-aligned)
  - Quality options (radio-style, single selection, current highlighted):
    - LOW | MEDIUM | **HIGH** | ULTRA
    - Click → `controller.setQuality(option)`, closes popup
  - Separator
  - VSR row: "AI Super Resolution" (left) + toggle switch (right)
    - Switch state: `controller.vsrActive`
    - Toggle → `controller.toggleVsr()`
    - Does NOT close popup (user may want to adjust scale after toggling)
  - Scale row: "Scale" label + options (horizontal, visible only when VSR on)
    - 1× | 1.5× | 2× | 3× | 4×
    - Radio-style, current highlighted
    - Click → `controller.setScale(n)`, closes popup
- When VSR is toggled OFF: scale row hides immediately, quality options remain selectable
- When VSR is toggled ON: scale row appears, defaults to auto-computed scale

```qml
Popup {
    id: qualityPopup
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    width: 280
    padding: 16
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: colorPanelBg; radius: 8
        border { width: 1; color: colorPanelBorder }
    }

    Column {
        spacing: 8
        anchors.fill: parent

        Text { text: "Quality"; color: colorTextSecondary; font.pixelSize: 13 }

        Repeater {
            model: ["LOW", "MEDIUM", "HIGH", "ULTRA"]
            delegate: qualityDelegate
        }

        Rectangle { width: parent.width; height: 1; color: colorSeparator }

        Row {
            spacing: 12
            Text { text: "AI Super Resolution"; color: colorTextPrimary; font.pixelSize: 14
                   anchors.verticalCenter: parent.verticalCenter }
            // Toggle switch — sets vsr on/off
        }

        // Scale options (visible when vsr enabled)
        Row {
            spacing: 8
            visible: controller && controller.vsrActive
            Repeater {
                model: [1, 2, 3, 4]
                delegate: scaleDelegate
            }
        }
    }
}
```

### 5.8 SpeedPopup

- Trigger: click Speed button → toggle
- Position: centered on video
- Size: 180×auto
- Background: `colorPanelBg`, radius 8px
- Content:
  - Title: "Speed" (13px, `colorTextSecondary`)
  - Options: 0.5× | 0.75× | **1×** | 2× (radio-style, current highlighted in `colorItemActive`)
- Click option → `controller.setSpeed(value)`
- Click outside → close

### 5.9 PlaylistPanel

- Trigger: Playlist button click
- Position: right edge, 320px wide, full height, slide animation
- Background: `colorPanelBg`
- Header:
  - Title: "Playlist" (15px bold, `colorTextPrimary`)
  - File count: "N files" (12px, `colorTextSecondary`)
  - Close button: Material `close` icon, top-right
- List: `ListView`, clip: true
  - Active item: `colorItemHover` background, `colorTextPrimary` text
  - Inactive: transparent, `colorTextSecondary` text
  - Click → `controller.loadFile(modelData)`
- Dismiss: close button, playlist button toggle, Escape key, click outside

### 5.10 Fullscreen Button

- Icon: `fullscreen` / `fullscreen_exit` (toggles based on window state)
- Click → `window.visibility = (window.visibility === Window.FullScreen) ? Window.Windowed : Window.FullScreen`
- No popup

## 6. Visibility & Auto-Hide

```qml
// overlay.qml root
MouseArea {
    id: visibilityArea
    anchors.fill: parent
    hoverEnabled: true
    // Show on enter, start hide timer on exit
    onEntered: { overlaysVisible = true; hideTimer.stop() }
    onExited: hideTimer.restart()
}

Timer {
    id: hideTimer
    interval: 3000
    onTriggered: overlaysVisible = false
}

property bool overlaysVisible: true
```

Key design:
- **Enter overlay area** → immediately show, kill hide timer
- **Exit overlay area** → start 3s countdown
- **No movement detection** needed — `hoverEnabled` + `containsMouse` already handles this
- The auto-hide applies to: TopBar, BottomBar, ProgressSlider. CenterPlayButton has its own visibility logic (tied to pause state).
- PlaylistPanel is excluded from auto-hide (user explicitly opened it).

## 7. Keyboard Shortcuts

**All shortcuts handled in KeyFilter.cpp only.** QML Keys.onPressed removed entirely to eliminate duplication.

| Key | Action |
|-----|--------|
| Space | Play/Pause |
| Escape | Close playlist if open, else stop |
| Left | Seek -5s |
| Right | Seek +5s |
| Shift+Left | Seek -10s |
| Shift+Right | Seek +10s |
| Up | Volume +5% |
| Down | Volume -5% |
| F | Toggle fullscreen |
| S | Screenshot |
| N | Next in playlist |
| B | Previous in playlist |
| P | Toggle playlist panel |
| [ | Speed 0.5× |
| ] | Speed 2.0× |
| \ | Speed 1.0× |

## 8. QuickController — Q_PROPERTY Additions

```cpp
class QuickController : public QObject {
    Q_OBJECT
    // Existing
    Q_PROPERTY(bool playing ...)
    Q_PROPERTY(int64_t currentTime ...)
    Q_PROPERTY(int64_t duration ...)
    Q_PROPERTY(QString videoInfo ...)
    Q_PROPERTY(bool vsrActive ...)
    Q_PROPERTY(bool hwDecoding ...)
    Q_PROPERTY(QString pendingFile ...)
    Q_PROPERTY(int pendingScale ...)
    Q_PROPERTY(double speed ...)
    // Added
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString quality READ quality NOTIFY qualityChanged)
    Q_PROPERTY(int scale READ scale NOTIFY scaleChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(QString pendingQuality READ pendingQuality NOTIFY pendingChanged)

public slots:
    // Existing
    void togglePlayPause();
    void stop();
    void seek(int64_t offsetMs);
    void seekAbsolute(int64_t ms);
    void setVolume(double vol);
    void setQuality(const QString& q);
    void toggleMute();
    void screenshot();
    void loadFile(const QString& path);
    void setSpeed(double speed);
    // Added
    void setScale(int s);
    void toggleVsr();
    void setFullscreen(bool fs);
```

## 9. File Change Matrix

| File | Action | Description |
|------|--------|-------------|
| `src/client/overlay.qml` | **Rewrite** | New layout, all components redesigned |
| `src/client/IconButton.qml` | **Rewrite** | Material Icons font-based, declarative hover |
| `src/client/IconPaths.qml` | **Delete** | Replaced by Material Icons font |
| `src/client/QuickController.h` | **Modify** | Add volume/muted/quality/scale/fullscreen Q_PROPERTYs, setScale/toggleVsr/setFullscreen slots |
| `src/client/QuickController.cpp` | **Modify** | Implement new slots, forward to PlayerCore via send_command |
| `src/client/KeyFilter.h` | **Modify** | Add volume tracking (already has vol_), add playlist open state |
| `src/client/KeyFilter.cpp` | **Modify** | Escape: close playlist → stop; remove duplicate keys handled in QML |
| `src/client/main.cpp` | **Modify** | Load Material Icons font; wire new controller properties |
| `src/client/shaders/` | **Keep** | Unchanged |
| `third_party/` | **Add** | Material Icons font file (Apache 2.0) |
| `Makefile` | **Modify** | Bundle font file, update QRC if used |

## 10. Material Icons Font Bundling

Option A (recommended): System package `ttf-material-icons` (Arch: `otf-material-icons`).
Fallback: Bundle `MaterialIcons-Regular.ttf` in `third_party/fonts/`.

The `FontLoader` tries the bundled path first, falls back to system path.

## 11. Deferred Items

- Settings panel (settings gear) — removed from this spec. All toggles are direct buttons.
- Seek thumbnail hover — deferred (requires frame decode at arbitrary position).
- Buffer indicator on progress bar — requires decoder to report buffered range (API exists but not wired).
