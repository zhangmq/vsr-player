# Overlay UI & Playlist — Design Spec

**Date:** 2026-06-23
**Status:** Approved
**Branch:** TBD (to be created)

## Overview

Replace the current minimal MainWindow UI with a full overlay-based player interface inspired by YouTube/B站 fullscreen mode. All controls are overlaid on the video surface with auto-hide behavior. Add a playlist panel that slides in from the right, populated by folder traversal.

## UI Architecture

### Overlay Layers (bottom-to-top)

```
┌──────────────────────────────────────────┐
│  Video Surface (VulkanWidget)             │
│  ┌──────────────────────────────────────┐ │
│  │ Top Bar (48px)                       │ │
│  │   filename                            │ │
│  └──────────────────────────────────────┘ │
│  ┌──────────────────────────────────────┐ │
│  │ Center Play Button (72×72, paused)    │ │
│  └──────────────────────────────────────┘ │
│  ┌──────────────────────────────────────┐ │
│  │ Bottom Control Bar (56px)             │ │
│  │ ◀ ▶ ⏸ ■ ──●── 00:00/00:00 🔊 Q NV ⚙ ☰ │ │
│  └──────────────────────────────────────┘ │
│  ┌────────────────────────────┐           │
│  │ Playlist Panel (320px)     │ ← slide   │
│  │ semi-transparent black     │           │
│  └────────────────────────────┘           │
└──────────────────────────────────────────┘
```

### Visibility Behavior

| Element | Trigger | Hide |
|---------|---------|------|
| Top Bar | Mouse movement / cursor near top edge | Fade out after 3s idle |
| Bottom Bar | Mouse movement / cursor near bottom edge | Fade out after 3s idle |
| Center Play Button | Playback paused AND overlays visible | When playing or overlays hidden |
| Playlist Panel | 📋 button click or keyboard shortcut | ✕ button, click outside, or toggle |

### Color Palette

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

### 1. Top Bar (`TopBar`)

- **Position:** Top of window, 48px height
- **Content:** Current filename, left-aligned
- **Visibility:** Auto-hide after 3s mouse idle; show on mouse movement or cursor in top 60px
- **No buttons** — settings and playlist toggles live in the bottom bar only

### 2. Bottom Control Bar (`ControlBar`)

- **Position:** Bottom of window, 56px height
- **Content (left to right):**

| # | Control | Icon | Size | Tooltip | Player API |
|---|---------|------|------|---------|------------|
| 1 | Previous | ◀ skip-back (solid) | 22×22 | "上一个" | `LOAD_FILE` (previous in playlist) |
| 2 | Play/Pause | ▶ / ⏸ (solid) | 28×28 | "播放/暂停 (Space)" | `PLAY` / `PAUSE` |
| 3 | Next | ▶ skip-forward (solid) | 22×22 | "下一个" | `LOAD_FILE` (next in playlist) |
| 4 | Stop | ■ square (solid) | 20×20 | "停止" | `STOP` |
| 5 | Progress Bar | seekable slider with thumb | flex | — | `SEEK` |
| 6 | Time | `MM:SS / MM:SS` | text 13px | — | — |
| 7 | Volume | 🔊 speaker (solid body + 1 arc) | 22×22 | "音量" | `SET_VOLUME` |
| 8 | Quality | Text label: "HIGH" | 13px + badge | "画质/VSR" | `SET_QUALITY` + VSR toggle |
| 9 | HW/SW | Text label: "NVDEC" / "SW" | 13px + badge | "硬解/软解" | Toggle hwaccel (rebuild pipeline) |
| 10 | Settings | ⚙ gear (solid) | 20×20 | "设置" | Opens `SettingsMenu` |
| 11 | Playlist | ☰ list (solid dots + lines) | 20×20 | "播放列表" | Toggles `PlaylistPanel` |

### 3. Settings Menu (`SettingsMenu`)

- Popup anchored above the settings button
- **Items:**
  - VSR: OFF / LOW / MEDIUM / HIGH / ULTRA (radio group)
  - Scale: 1× / 1.5× / 2× / 3× / 4× (if VSR enabled)
  - Separator
  - Screenshot (CAPTURE_FRAME)
- Dismisses on click outside or selection

### 4. Playlist Panel (`PlaylistPanel`)

- **Position:** Right side, 320px width, full height
- **Animation:** Slide in/out from right (QPropertyAnimation, ~200ms)
- **Background:** Semi-transparent black `rgba(0,0,0,0.85)` with backdrop blur
- **Header:** "播放列表" title + "N files · depth D" info + ✕ close button
- **List:** Flat file list with vertical scrollbar
  - Active item: `rgba(255,255,255,0.06)` background highlight (no arrow indicator)
  - Inactive items: `#999` text
  - Click to load file via `LOAD_FILE`
- **Scrollbar style:** Dark theme, 6px wide, subtle white handle
- **Close:** ✕ button, 📋 toggle, click outside panel, or Escape key

### 5. Center Play Button

- Visible only when paused AND overlays are shown
- 72×72 circle, translucent white border, centered on video
- Click → `PLAY`

### 6. Playlist Engine (`PlaylistEngine` — non-UI)

- **Input:** Root directory path + traversal depth
- **Traversal:** BFS/DFS up to `--depth` (default 3). Sorted by filename, flattened.
- **Filter:** Common video extensions (`.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.ts`, `.m2ts`, `.flv`, `.wmv`). FFmpeg demuxer validates on open, skip on failure.
- **CLI:** Smart path detection — if argument is a directory → playlist mode; if file → single video mode.
- **API:** `scan_folder(path, depth)` → `QStringList`, `current_index()`, `next()`, `previous()`

## Icon System

**Approach:** QPainter SVG path rendering via a centralized `IconProvider` class.

```cpp
// IconProvider — single source of truth for all icons
class IconProvider {
public:
    static QPixmap icon(IconName name, int size, QColor color);
    // Icons: SkipBack, Play, Pause, SkipForward, Stop,
    //        Volume, VolumeMuted, Settings, Playlist, Close
private:
    static QString pathFor(IconName name); // SVG path string
};
```

All icons: **solid fill**, stroke width 2 for outline details, color `#c8c8c8` default / `#ffffff` hover.

Paths sourced from Feather Icons (MIT) where available; custom/adjusted for solid variants.

## CLI Changes

```
vsr-player [--no-vsr] [--no-hwaccel] [--quality LOW|MEDIUM|HIGH|ULTRA]
           [--depth N] <path>
```

- `<path>`: If directory → playlist mode (scan folder). If file → single video mode.
- `--depth N`: Traversal depth for folder scan (default 3). Ignored for single file.
- Existing flags (`--no-vsr`, `--no-hwaccel`, `--quality`) unchanged.

## File Change Matrix

| File | Action | Description |
|------|--------|-------------|
| `src/client/MainWindow.h/cpp` | **Rewrite** | Overlay container, auto-hide timer, layout management, event wiring |
| `src/client/ControlBar.h/cpp` | **Rewrite** | All bottom bar controls, QPainter icon buttons, progress/slider |
| `src/client/PlaylistPanel.h/cpp` | **Rewrite** | Slide-in panel, file list, scrollbar styling |
| `src/client/PlaylistEngine.h/cpp` | **New** | Folder traversal, file filtering, playlist navigation |
| `src/client/SettingsMenu.h/cpp` | **New** | Quality/VSR popup menu |
| `src/client/IconProvider.h/cpp` | **New** | Centralized QPainter icon rendering |
| `src/client/TopBar.h/cpp` | **New** | Filename display, auto-hide |
| `src/client/main.cpp` | **Modify** | Add `--depth N`, smart path detection |
| `src/client/VulkanWidget.h/cpp` | **Keep** | No changes |
| `src/core/api/Player.h` | **Evaluate** | May need `SET_VSR_ENABLED`, `TOGGLE_HWACCEL` commands |
| `src/client/StatusBar.h/cpp` | **Delete or merge** | Functionality moved to ControlBar overlay |
| `src/client/SettingsDialog.h/cpp` | **Replace** | QDialog → overlay SettingsMenu |
| `src/client/PlayerProxy.h/cpp` | **Delete** | Unused; MainWindow talks to Player directly |

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
- **HW/SW toggle failure:** Pipeline rebuild fails → revert to previous state, show error tooltip

## Testing

| Test | Method |
|------|--------|
| Icon rendering | Visual inspection, all sizes and colors |
| Auto-hide behavior | Manual: move mouse → show; idle 3s → fade |
| Playlist scan | Unit test: `PlaylistEngine::scan_folder()` with test dir tree |
| Playlist navigation | Manual: previous/next cycle through list |
| Quality switch | Manual: change quality → verify VSR reconfig |
| HW/SW toggle | Manual: toggle → verify pipeline rebuild, no crash |
| CLI parsing | Integration: `--folder`, `--depth`, smart path detection |
| Window close with overlay | Verify no crash (existing closeEvent logic preserved) |
