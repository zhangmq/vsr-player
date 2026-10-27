# 3D and Single-Eye

VSR Player can drive XR glasses that take a side-by-side (SBS) framebuffer, and — for when
you would rather watch flat — output **one eye** as an ordinary full-frame picture.

## Output modes

The right-click menu has a small **3D** group with three independent entries.

| Entry | Values | Meaning |
|---|---|---|
| **Output mode** | Off · 3D glasses · 2D output | What goes to the screen |
| **Source layout** | Side by side · full / half, Top and bottom · full / half | How the *source* packs the two eyes |
| **Output option** | Left eye / Right eye | Which eye "2D output" shows |

The three entries are deliberately independent — choosing a source layout does not change
the output mode and vice versa. Only "Output mode" itself is a switch: picking 3D glasses
starts (and stops the single-eye mode), picking 2D output does the opposite.

## 3D glasses mode

- Picking **3D glasses** opens a screen picker (physical resolution, remembered per screen)
  and moves the video window onto that display as a fullscreen presentation window.
- The desktop keeps its own window with the controls (playlist, quality, 3D menu), so the
  UI does not disappear.
- `H` shows the hotkey overlay on the glasses; `J` toggles SBS/TAB, `K` swaps the eyes, `L`
  toggles full/half.
- The presentation window ignores focus and right-click; leaving 3D puts the video window
  back where it was.

**Geometry.** The output framebuffer is always SBS, so the per-eye viewport is
`outW/2 × outH` and its aspect is `outAspect / 2`. Per-eye content size depends on the source
layout:

| Source layout | Eye image |
|---|---|
| SBS · full | `srcW/2 × srcH`, copied 1:1 |
| SBS · half | `srcW × srcH` (the eye is stretched along the split axis) |
| TAB · full | `srcW × srcH/2` |
| TAB · half | `srcW × srcH` |

The compositor then contains the eye into its viewport (no stretching, black bars if the
aspect does not match). Split coordinates are handled in **integer pixels** and the render
target width is rounded up to an even number, so both eyes sample an identical grid — a
half-pixel difference would show up as a constant horizontal parallax.

**Aspect ratio in 3D.** The bottom-bar popup and the `A` hotkey set a *per-eye* aspect. mpv's
`video-aspect-override` applies to the whole frame, so the value is converted before it is
sent: SBS·full = ×2, SBS·half = ×1, TAB·full = ÷2, TAB·half = ×1. For ordinary 2D playback
the value is passed through unchanged.

## Single-eye ("2D output") mode

Useful when you just want to watch the film: the player takes one eye of the SBS/TAB source
and presents it as a normal picture, so it fills an ordinary window and can be watched
without glasses.

- The `vf_eye` filter runs **before** the upscaler, so VSR works on the per-eye picture
  (a 1080p-per-eye source gets 1:1 output instead of a squeezed two-eyes-wide frame).
- **Half** sources are stretched back to natural eye size by the filter itself (not by a
  pixel aspect ratio), so aspect override, rotation, auto-scaled VSR and interpolation all
  behave exactly like normal 2D.
- No 3D presentation window, no second display: the picture stays in the normal player
  window, and the desktop UI keeps working as usual.
- `K` switches to the other eye while in this mode.

The same filter is available in the `mpv-vsr` CLI and through the JSON IPC
(`set-eye off|left|right [sbs|tab] [full|half]`, `get-eye`).

## Auto-scaling with 3D sources

With `scale=auto`, the upscale factor is computed from **per-eye** geometry, not from the
whole side-by-side frame:

```
eyeW = outW/2, eyeH = outH                     # framebuffer is always SBS
imgW,imgH = per-eye source pixels (table above)
need = min(eyeW/imgW, eyeH/imgH)               # then snapped to the SDK set 4/3, 1.5, 2, 3, 4
```

Measured on a 3840×1080 framebuffer at 60 fps output (RTX 5060 Ti):

| Source | Factor | VSR cost | Result |
|---|---|---|---|
| 3840×1080 SBS full (1080p/eye) | 1.00 | passthrough | 60.0 fps, 0 dropped |
| 1280×720 SBS full | 1.50 | 3.4 ms | 60.0 fps, 0 dropped |
| 960×720 TAB full | 2.00 | 2.3 ms | 60.0 fps, 0 dropped |
| 640×360 SBS full | 3.00 | 1.0 ms | 60.0 fps, 0 dropped |

3D content at 1080p per eye plus ×2 interpolation runs at about 13 % GPU: the expensive
cases are low-resolution sources, not the 3D path itself.

## Converting 3D Blu-ray to SBS

MVC (H.264 Stereo High) discs are not decoded in the player on purpose — the offline route
is simpler and more reliable. `docs/handover-3d-conversion.md` describes the pipeline
(`ssif_to_mvc.py` + `mvc2sbs`) that turns a 3D Blu-ray into full-width SBS files playable
with "3D glasses · Side by side · full".
