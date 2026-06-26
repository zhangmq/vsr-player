# Overlay QML Component Refactor — Design Spec

**Date:** 2026-06-26
**Branch:** `feature/overlay-component-refactor`
**Status:** Approved

## ⛔ Hard Constraint

**This is a pure structural refactoring. No functional changes, no behavioral adjustments, no visual tweaks — of any kind.**

Every line of QML that produces a visual or behavioral outcome MUST be preserved character-by-character within a component file. The only permitted changes are:

1. Extracting a block from `overlay.qml` into its own file
2. Replacing the extracted block with a component instantiation + property bindings
3. Replacing internal `id` cross-references with property/signal wiring through overlay.qml

Prohibited:
- Changing colors, sizes, fonts, spacing, durations, easing curves
- Adding/removing features, interactions, or visual elements
- Refactoring internal logic (even if "better")
- Fixing bugs, warnings, or Qt skill rule violations found during extraction
- Reordering, renaming, or reformatting anything beyond what extraction requires

## Goal

Split `src/client/ui/overlay.qml` (587-line single file) into focused components without changing any visual behavior or functionality. Zero regression.

## Approach: Medium split (B)

~10 files total. Three popups, playlist, bottom bar, progress slider, top bar, center button, OSD as independent components. Reusable `IconButton` extracted to `components/`. `overlay.qml` becomes a thin wiring layer.

## File Layout

```
src/client/ui/
├── overlay.qml              ← entry: layout + wiring only
├── TopBar.qml
├── CenterPlayBtn.qml
├── OsdOverlay.qml
├── ProgressSlider.qml
├── BottomBar.qml
├── VolumePopup.qml
├── QualityPopup.qml
├── SpeedPopup.qml
├── PlaylistPanel.qml
└── components/
    └── IconButton.qml
```

## Component Specs

### IconButton (`components/IconButton.qml`)

Reusable. No parent dependencies.

```
Properties:  codepoint, size (22), tooltip (""), highlighted (false), label ("")
Signal:      clicked()
```

Used by: BottomBar, VolumePopup, PlaylistPanel close button.

### TopBar (`TopBar.qml`)

```
Properties:  videoInfo (string), visible (bool)
```

Gradient background with left-aligned text. No signals.

### CenterPlayBtn (`CenterPlayBtn.qml`)

```
Properties:  playing (bool), visible (bool)
Signal:      clicked()
```

### OsdOverlay (`OsdOverlay.qml`)

```
Properties:  visible (bool), text (string)
```

Semi-transparent rounded rect with monospace text. No signals.

### ProgressSlider (`ProgressSlider.qml`)

Thin seek bar with hover highlight, conditional handle, seek-on-release. The invisible hot zone above it (for auto-hide prevention) stays in overlay.qml — it's a layout concern, not part of the slider.

```
Properties:  duration (real), currentTime (real), visible (bool)
Signal:      seeked(ms)
```

### BottomBar (`BottomBar.qml`)

Gradient background. Left group: prev/play/next/stop + time label. Right group: volume/quality/hwaccel/speed/fullscreen/playlist.

Exposes button center X coordinates as readonly properties for popup positioning in overlay.qml.

```
Properties:  playing, fullscreen, hwDecoding, currentTime, duration, overlaysVisible,
             volumePopupOpen, qualityPopupOpen, speedPopupOpen  (for button highlighted state)
Readonly:    volumeBtnCenterX, qualityBtnCenterX, speedBtnCenterX (real)
Signals:     playPauseClicked(), prevClicked(), nextClicked(), stopClicked(),
             volumeClicked(), qualityClicked(), hwaccelClicked(), speedClicked(),
             fullscreenClicked(), playlistClicked()
```

### VolumePopup (`VolumePopup.qml`)

```
Properties:  x, y, volume, muted
Signals:     volumeChanged(real), muteToggled(), dismissed()
```

Vertical slider + mute toggle. Self-manages `visible` via internal Popup; opening/closing controlled from overlay.qml via `open()`/`close()`.

### QualityPopup (`QualityPopup.qml`)

```
Properties:  x, y, scale, quality, denoiseQuality, scaleModel, levelModel, denoiseModel
Signals:     scaleChanged(int), qualityChanged(int), denoiseQualityChanged(int), dismissed()
```

Note: `scaleModel`/`levelModel`/`denoiseModel` are the JS arrays currently defined in overlay.qml root. They move here since only QualityPopup uses them.

### SpeedPopup (`SpeedPopup.qml`)

```
Properties:  x, y, speed
Signals:     speedChanged(real), dismissed()
```

### PlaylistPanel (`PlaylistPanel.qml`)

Right-edge Drawer with header and ListView. Directly binds `playlist` context property (same as before).

```
Properties:  none (self-contained, reads playlist from root context)
Signals:     fileSelected(path), dismissed()
```

## Wiring in overlay.qml

```
overlay.qml
  ├── FontLoader (material icons)
  ├── Auto-hide MouseArea + Timer
  ├── TopBar { videoInfo: viewModel.videoInfo; visible: viewModel.overlaysVisible }
  ├── CenterPlayBtn { playing: viewModel.playing; visible: !playing && overlaysVisible }
  ├── OsdOverlay { visible: viewModel.osdVisible; text: viewModel.osdText }
  ├── ProgressSlider { duration: viewModel.duration; currentTime: viewModel.currentTime; ... }
  ├── BottomBar { ... }
  ├── VolumePopup { x: calc; y: calc; ... }
  ├── QualityPopup { x: calc; y: calc; ... }
  ├── SpeedPopup { x: calc; y: calc; ... }
  ├── PlaylistPanel { ... }
  └── Keyboard shortcuts + Fullscreen sync (unchanged)
```

### Key wiring connections

```
bottomBar.volumeClicked    → volumePopup.open()
bottomBar.qualityClicked   → qualityPopup.open()
bottomBar.speedClicked     → speedPopup.open()
bottomBar.playlistClicked  → togglePlaylist()

volumePopup.dismissed      → volumePopup.visible = false
qualityPopup.dismissed     → qualityPopup.visible = false
speedPopup.dismissed       → speedPopup.visible = false

volumePopup.volumeChanged  → viewModel.setVolume(v)
volumePopup.muteToggled    → viewModel.toggleMute()
qualityPopup.scaleChanged  → viewModel.setScale(v)
qualityPopup.qualityChanged → viewModel.setQuality(v)
qualityPopup.denoiseQualityChanged → viewModel.setDenoiseQuality(v)
speedPopup.speedChanged    → viewModel.setSpeed(v)

// Button highlighted states
bottomBar.volumePopupOpen:  volumePopup.visible
bottomBar.qualityPopupOpen: qualityPopup.visible
bottomBar.speedPopupOpen:   speedPopup.visible

// Popup X positions computed from bottomBar readonly properties
volumePopup.x:  Math.min(Math.max(bottomBar.volumeBtnCenterX - w/2, 8), root.w - w - 8)
qualityPopup.x: Math.min(Math.max(bottomBar.qualityBtnCenterX - w/2, 8), root.w - w - 8)
speedPopup.x:   Math.min(Math.max(bottomBar.speedBtnCenterX - w/2, 8), root.w - w - 8)
```

### Auto-hide

Unchanged. `volumePopup.visible || qualityPopup.visible || speedPopup.visible` replaces the old `volumePopup.visible || qualityPopup.visible || speedPopup.visible` check — identical logic.

## What stays in overlay.qml

- FontLoader (referenced by IconButton via `root.iconFont`)
- Auto-hide MouseArea + Timer
- Layout positioning of all child components
- Central wiring (signals → signals, property bindings)
- Keyboard shortcuts (`Keys.onPressed` on root Item)
- Fullscreen bidirectional sync (`Connections` to `window` and `viewModel`)
- Dismiss click-outside for playlist (MouseArea overlay)
- `togglePlaylist()` function

## Build impact

None. Qt's QML engine resolves relative file references at load time. `QUrl::fromLocalFile` in `main.cpp` still points to `overlay.qml`. The `qmldir` is not needed — all components are referenced by filename in the same directory.

## Verification

1. Build: `make -j$(nproc)` (no C++ changes, should pass trivially)
2. Run player with a test video
3. Check each interaction:
   - Play/pause, prev, next, stop buttons
   - Progress slider seek
   - Volume popup open/close/change/mute
   - Quality popup open/close/change
   - Speed popup open/close/change
   - Hwaccel toggle
   - Fullscreen toggle
   - Playlist open/close/select
   - OSD toggle (Tab key)
   - Auto-hide behavior (mouse move, timer)
   - Keyboard shortcuts (Space, P, F, Esc)
   - Center play button visibility
