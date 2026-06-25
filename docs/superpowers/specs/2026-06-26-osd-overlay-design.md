# OSD Overlay — Design Spec

**Date:** 2026-06-26  
**Branch:** `feature/osd-overlay`

**Goal:** Real-time playback information overlay, toggled with Tab, left-aligned, updated ~2×/s.

## Data

Every 30 frames (~0.5s at 60fps), Core emits a `FRAME_INFO` event with:

| Field | Source | Notes |
|-------|--------|-------|
| `in_width` / `in_height` | `video_w_` / `video_h_` | existing |
| `out_width` / `out_height` | `vsr_w_` / `vsr_h_` | existing |
| `fps` | `video_fps_` | existing |
| `scale` | `current_scale_` | existing |
| `quality` | `quality_` | existing |
| `denoise` | `denoise_quality_` | existing |
| `hw_decoding` | `decoder_->is_hardware()` | existing |
| `speed` | `playback_speed_` | existing |
| `pts_ms` / `duration_ms` | `current_pts_sec_` / `duration_ms_` | existing |
| `frame_idx` | new counter (`frame_count_`) | incremented per frame |
| `render_fps` | `frame_count_ / elapsed` | rolling average since last FRAME_INFO |
| `gpu_name` | `cuDeviceGetName` | cached on first call |
| `vram_mb` | `cuMemGetInfo` | used/total |
| `audio_sr` / `audio_ch` | `AudioOutput` probe result | cached |
| `codec_name` | `avcodec_get_name()` from `decoder_->active_codec_id()` | new getter |
| `pix_fmt` | active decoder pix_fmt string | new getter |
| `vsr_active` | `vsr_ != nullptr` | existing |

## Architecture

```
Core process_one_frame()
  → every 30 frames → emit FRAME_INFO
    → main.cpp event callback → viewModel.updateOsdInfo(event)
      → formats QString → QML Text binding
```

## Files

| File | Change |
|------|--------|
| `src/core/api/Player.h` | Extend `FRAME_INFO` fields in `PlayerEvent` |
| `src/core/PlayerCore.h` | Add `frame_count_`, `last_info_time_`, `gpu_name_`, `audio_sr_`, `audio_ch_` |
| `src/core/PlayerCore.cpp` | Emit `FRAME_INFO` every 30 frames in `process_one_frame()` |
| `src/core/Decoder.h/cpp` | Add `active_codec_id()` getter for codec name |
| `src/core/AudioOutput.h/cpp` | Expose sample rate / channels from probe |
| `src/client/PlayerViewModel.h/cpp` | Add `osdText`, `osdVisible` Q_PROPERTYs, `updateOsdInfo()` slot |
| `src/client/KeyFilter.cpp` | Add Tab key → toggle `osdVisible` |
| `src/client/ui/overlay.qml` | OSD Rectangle + Text, top-left, monospace |

## QML

```qml
// Top-left overlay
Rectangle {
    anchors { left: parent.left; top: parent.top; leftMargin: 16; topMargin: 16 }
    color: "#99000000"; radius: 6
    opacity: viewModel.osdVisible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 150 } }

    Text {
        text: viewModel.osdText
        color: "#e0e0e0"
        font.family: "monospace"; font.pixelSize: 12
        lineHeight: 1.4
        leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
        renderType: Text.NativeRendering
    }
}
```

## OSD Format

```
Source    1920×1080 h264 29.97fps
Output    3840×2160 (2×)
VSR       High quality / Denoise off
Decoder   NVDEC (hardware)
Speed     1.00×
PTS       0:03:42 / 0:10:00
Render    1440p RGBA → 4K window  60.1fps
GPU       RTX 5060 Ti  512/8188 MB
Audio     44100Hz 2ch
Frame     #2214
```

## Tab Toggle

- `KeyFilter::eventFilter` adds `Qt::Key_Tab` → `viewModel.toggleOsd()`
- Default: OSD off. Tab toggles.
- State persists per session (no need to save to file).

## Verification

1. Build passes
2. Run player → OSD hidden initially
3. Press Tab → OSD appears top-left, semi-transparent, monospace
4. Info updates ~every 0.5s (visible frame counter advancing, PTS changing)
5. Press Tab again → OSD hides with fade
6. GPU name, VRAM, codec correct for test file
