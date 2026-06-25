# PlayerViewModel + UI Component Architecture — Design Spec

**Date:** 2026-06-25
**Goal:** Centralize all UI state in a C++ Q_PROPERTY ViewModel, eliminate scattered
QML `property` declarations, and split overlay.qml into focused single-responsibility
component files in `src/client/ui/`.

## 1. Architecture

```
┌──────────────────────────────────────────────────┐
│ QML Layer (src/client/ui/)                               │
│                                                    │
│  overlay.qml  ←  thin root: FontLoader + layout    │
│  ├── TopBar.qml                                    │
│  ├── CenterPlayButton.qml                           │
│  ├── ProgressSlider.qml                             │
│  ├── BottomBar.qml  ←  all control buttons          │
│  │   ├── IconButton.qml (×10)                       │
│  │   ├── VolumePopup.qml                            │
│  │   ├── QualityPopup.qml (centered)                │
│  │   └── SpeedPopup.qml (centered)                   │
│  └── PlaylistPanel.qml                              │
│                                                    │
│  ALL state reads from PlayerViewModel Q_PROPERTYs   │
│  ALL actions call PlayerViewModel slots             │
│  ZERO QML-side property declarations for state      │
├──────────────────────────────────────────────────┤
│ PlayerViewModel (C++, exposed via contextProperty) │
│                                                    │
│  Q_PROPERTY(bool playing ...)                      │
│  Q_PROPERTY(int64_t currentTime ...)                │
│  Q_PROPERTY(int64_t duration ...)                   │
│  Q_PROPERTY(bool overlaysVisible ...)               │
│  Q_PROPERTY(double volume ...)                      │
│  Q_PROPERTY(bool muted ...)                         │
│  Q_PROPERTY(QString quality ...)                    │
│  Q_PROPERTY(bool vsrActive ...)                     │
│  Q_PROPERTY(int scale ...)                          │
│  Q_PROPERTY(double speed ...)                       │
│  Q_PROPERTY(bool hwDecoding ...)                    │
│  Q_PROPERTY(QString videoInfo ...)                  │
│  Q_PROPERTY(QString pendingFile ...)                │
│  Q_PROPERTY(int pendingScale ...)                   │
│  Q_PROPERTY(bool fullscreen ...)                    │
│                                                    │
│  public slots:                                      │
│    togglePlayPause(), stop(),                       │
│    seekAbsolute(ms), seekRelative(offsetMs),        │
│    setVolume(v), setQuality(q), setSpeed(s),        │
│    setScale(n), toggleVsr(), toggleMute(),          │
│    toggleFullscreen(), loadFile(path)                │
├──────────────────────────────────────────────────┤
│ PlayerCore (existing, no changes)                   │
└──────────────────────────────────────────────────┘
```

## 2. PlayerViewModel (C++, `src/client/PlayerViewModel.h`)

Evolves from `QuickController`. Renamed for clarity.

### Q_PROPERTY declarations

| Property | Type | Read | Write | Notify |
|----------|------|------|-------|--------|
| playing | bool | playing_ | — | playingChanged |
| currentTime | int64_t | currentTime_ | — | currentTimeChanged |
| duration | int64_t | duration_ | — | durationChanged |
| overlaysVisible | bool | overlaysVisible_ | setOverlaysVisible | overlaysVisibleChanged |
| volume | double | volume_ | setVolume | volumeChanged |
| muted | bool | muted_ | — | mutedChanged |
| quality | QString | quality_ | — | qualityChanged |
| vsrActive | bool | vsrActive_ | — | vsrActiveChanged |
| scale | int | scale_ | — | scaleChanged |
| speed | double | speed_ | — | speedChanged |
| hwDecoding | bool | hwDecoding_ | — | hwDecodingChanged |
| videoInfo | QString | videoInfo_ | — | videoInfoChanged |
| pendingFile | QString | pendingFile_ | — | pendingChanged |
| pendingScale | int | pendingScale_ | — | pendingChanged |
| fullscreen | bool | fullscreen_ | setFullscreen | fullscreenChanged |

### Slots

| Slot | Action |
|------|--------|
| `togglePlayPause()` | Flip `playing_`, emit, send CmdPlay/CmdPause |
| `stop()` | Set `playing_=false`, emit, send CmdStop |
| `seekAbsolute(int64_t ms)` | Send CmdSeek{ms} (absolute position) |
| `seekRelative(int64_t offsetMs)` | Compute `currentTime_ + offsetMs`, clamp [0, duration], call seekAbsolute |
| `setVolume(double v)` | Update `volume_`, emit; if v<0.01 set muted_=true; send CmdSetVolume |
| `setQuality(QString)` | Parse string → Quality enum, update `quality_`, emit, send CmdSetQuality |
| `setSpeed(double s)` | Update `speed_`, emit, send CmdSetSpeed |
| `setScale(int s)` | Update `scale_`, emit, send CmdSetScale |
| `toggleVsr()` | Flip `vsrActive_`, emit, send CmdSetVsr |
| `toggleMute()` | Flip `muted_`, emit, send CmdSetMute |
| `toggleFullscreen()` | Flip `fullscreen_`, emit; QML reads this prop to toggle window.visibility |
| `loadFile(path)` | Send CmdStop + CmdLoadFile |

### State sync from core events

```cpp
// Called from main.cpp event callback (QueuedConnection)
void updateState(bool playing);       // STATE_CHANGED
void updateTime(int64_t t, int64_t d); // POSITION_CHANGED
void updateVideoInfo(const QString&);  // VIDEO_INFO → also updates scale, vsr, hw
void updateVsrActive(bool);           // VIDEO_INFO
void updateHwDecoding(bool);          // VIDEO_INFO
void updateQuality(const QString&);   // FRAME_INFO
void updateScale(int);                // FRAME_INFO
void onPending(const PlayerEvent&);   // OPERATION_PENDING
```

### Key design rules

1. **Slots update their corresponding Q_PROPERTY BEFORE sending the command.**
   This gives instant optimistic UI feedback. Example:
   ```cpp
   void setSpeed(double s) {
       if (speed_ != s) { speed_ = s; emit speedChanged(); }
       player_->send_command(CmdSetSpeed{s});
   }
   ```

2. **Event callbacks update Q_PROPERTYs after the fact.**
   Core events carry authoritative state. If the core responds with a different value
   (e.g., quality negotiation), the event wins over the optimistic set.

3. **`overlaysVisible` is WRITE from QML, READ everywhere else.**
   QML's MouseArea sets it on enter/exit. Components bind opacity to this property.

4. **`fullscreen` is WRITE from QML when window.visibility changes.**
   Fullscreen button reads this property to show the correct icon.

5. **No QML-side `property` declarations for application state.**
   Zero. Every piece of state is a ViewModel Q_PROPERTY.

## 3. Component File Structure

```
src/
├── client/
│   ├── ui/                              ← ALL QML files
│   │   ├── overlay.qml                  # Root: FontLoader + layout composition
│   │   ├── IconButton.qml               # Reusable button (Material Icons font)
│   │   ├── TopBar.qml                   # Top gradient bar + filename text
│   │   ├── CenterPlayButton.qml         # 72×72 circle, paused only
│   │   ├── ProgressSlider.qml           # Full-width slider above BottomBar
│   │   ├── BottomBar.qml                # Gradient bar + Row of controls
│   │   └── PlaylistPanel.qml            # Right slide-in list
│   ├── main.cpp
│   ├── PlayerViewModel.h / .cpp
│   ├── PlaylistEngine.h / .cpp
│   ├── QtVulkanContext.h / .cpp
│   └── KeyFilter.h / .cpp
└── core/                                ← Unchanged
```

Popups (Volume, Quality, Speed) are inlined in overlay.qml for coordinate mapping access.

### Component responsibilities

**overlay.qml** (~60 lines)
- Loads Material Icons font via FontLoader
- Instantiates all components in layout order
- No application logic, no state properties (except transient animation helpers if needed)
- Auto-hide: MouseArea onEnter/onExit → sets `viewModel.overlaysVisible`

**TopBar.qml** (~25 lines)
- Reads `viewModel.pendingFile`, `viewModel.videoInfo` for display text
- Opacity bound to `viewModel.overlaysVisible`

**CenterPlayButton.qml** (~30 lines)
- Visible when `!viewModel.playing && viewModel.overlaysVisible`
- Click → `viewModel.togglePlayPause()`

**ProgressSlider.qml** (~45 lines)
- Binds `to: viewModel.duration`, `value: viewModel.currentTime`
- `onMoved: viewModel.seekAbsolute(value)`

**BottomBar.qml** (~100 lines)
- Contains the Row of all control buttons
- Each button calls a viewModel slot or triggers a popup
- Popup triggering: button click toggles popup visibility (popup reads `viewModel` directly)

**IconButton.qml** (~50 lines)
- Props: `codepoint`, `size`, `tooltip`, `highlighted`
- `fontFamily` passed as property from parent's FontLoader

**VolumePopup.qml** (~50 lines)
- Position: computed from parent BottomBar coordinates (`mapToItem`)
- Hover logic: button hover → open; leave button+popup → close after 300ms delay

### Scale=1 Denoise Logic

| Scale | VSR Switch | UI Label | Behavior |
|-------|-----------|----------|----------|
| =1 | OFF by default | "Denoise: OFF" | Passthrough, no GPU processing |
| =1 | User enables | "Denoise: LOW/MEDIUM/HIGH" | VSR at 1× (denoise only, no upscale) |
| >1 | ON by default | "AI Super Resolution: ON" + Quality | Upscale + denoise at selected quality |
| >1 | User disables | "AI Super Resolution: OFF" | NV12→RGB direct path, no VSR |

Quality sub-options are shared between both modes (quality level controls denoise intensity).

**QualityPopup.qml** (~100 lines)
- Position: above quality button on BottomBar, right-aligned with right edge of window
- Uses `mapToItem` for coordinate conversion from button → root
- Quality options: LOW/MEDIUM/HIGH/ULTRA → `viewModel.setQuality(name)`
- VSR toggle: Switch bound to `viewModel.vsrActive`, `onToggled: viewModel.toggleVsr()`
- Scale options (visible when VSR on): 1×/2×/3×/4× → `viewModel.setScale(n)`

**SpeedPopup.qml** (~50 lines)
- Position: above speed button on BottomBar, right-aligned with right edge of window
- Uses `mapToItem` for coordinate conversion
- Speed options: 0.5×/0.75×/1×/2× → `viewModel.setSpeed(val)`
- Current speed highlighted via `viewModel.speed === modelData`

**PlaylistPanel.qml** (~80 lines)
- Right slide-in, 320px width
- Z-order explicitly above all other elements
- Dismiss: click outside, Escape, close button, P key

## 4. Coordinate Mapping for Popup Positioning

Buttons inside BottomBar → Row use Row-relative coordinates.
Popups are children of root → need root-relative coordinates.

```qml
// In BottomBar.qml or overlay.qml
function buttonRootX(button) {
    return button.mapToItem(root, 0, 0).x
}
function buttonRootY(button) {
    return button.mapToItem(root, 0, 0).y
}
```

VolumePopup position:
```qml
x: buttonRootX(volumeBtn) + volumeBtn.width / 2 - width / 2
y: buttonRootY(volumeBtn) - height - 8
```

## 5. Bug Fixes

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| Seek slider no-op | No confirmed root cause — `onMoved: controller.seekAbsolute(value)` is correct. May be Slider event consumed by parent MouseArea. | Ensure ProgressSlider MouseArea is not overlapped. Test with debug log in seekAbsolute slot. |
| Close button dead | IconButton implicit size + z-order inside playlist header | Set explicit z on close button; verify MouseArea is on top |
| Popups centered | Hardcoded `(parent.width-width)/2` | Use `mapToItem` to compute button-relative position |
| `seek(offsetMs)` broken | Passed relative offset to `CmdSeek.position_ms` which is absolute | Rename to `seekRelative(offsetMs)`, compute `currentTime_ + offset` internally |
| Volume popup x wrong | `volumeWrapper.x` is Row-relative | Use `mapToItem` |

## 6. File Change Matrix

| File | Action | Description |
|------|--------|-------------|
| `src/client/PlayerViewModel.h` | **New** (from QuickController.h) | Full ViewModel with all Q_PROPERTYs |
| `src/client/PlayerViewModel.cpp` | **New** (from QuickController.cpp) | Slots + event handlers |
| `src/client/QuickController.h` | **Delete** | Replaced by PlayerViewModel |
| `src/client/QuickController.cpp` | **Delete** | Replaced by PlayerViewModel |
| `src/client/KeyFilter.h` | **Modify** | Use PlayerViewModel* instead of QuickController* |
| `src/client/KeyFilter.cpp` | **Modify** | Use PlayerViewModel slots; seekRelative |
| `src/client/main.cpp` | **Modify** | Instantiate PlayerViewModel; update QML source path; context property name "viewModel" |
| `src/client/ui/overlay.qml` | **New** | Thin root, FontLoader, layout composition |
| `src/client/ui/IconButton.qml` | **New** | Moved from src/client/ |
| `src/client/ui/TopBar.qml` | **New** | Extracted from old overlay.qml |
| `src/client/ui/CenterPlayButton.qml` | **New** | Extracted |
| `src/client/ui/ProgressSlider.qml` | **New** | Extracted |
| `src/client/ui/BottomBar.qml` | **New** | Extracted |
| `src/client/ui/VolumePopup.qml` | **New** | Extracted |
| `src/client/ui/QualityPopup.qml` | **New** | Extracted |
| `src/client/ui/SpeedPopup.qml` | **New** | Extracted |
| `src/client/ui/PlaylistPanel.qml` | **New** | Extracted |
| `src/client/overlay.qml` | **Delete** | Replaced by src/client/ui/overlay.qml |
| `src/client/IconButton.qml` | **Delete** | Moved to src/client/ui/ |
| `src/client/IconPaths.qml` | **Delete** | No longer used (Material Icons font) |
| `Makefile` | **Modify** | Update QML source paths, update object file paths |

## 7. Q_PROPERTY → QML Binding Examples

```qml
// BottomBar.qml
IconButton {
    iconText: ""  // play_arrow
    iconSize: 28
    highlighted: !viewModel.playing  // show play icon when paused
    fontFamily: materialIcons.name
    onClicked: viewModel.togglePlayPause()
}

// QualityPopup.qml
Switch {
    checked: viewModel.vsrActive
    onToggled: viewModel.toggleVsr()
}

// ProgressSlider.qml
Slider {
    from: 0
    to: viewModel.duration
    value: viewModel.currentTime
    onMoved: viewModel.seekAbsolute(value)
}
```

**No QML-side logic.** Every `if/else` is a `viewModel.property` read.
