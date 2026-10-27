# Video Processing

Everything the NVIDIA Video Effects SDK offers is wired into a single GPU filter chain
inside mpv:

- **AI super-resolution + denoise** — real-time 2×/3×/4× upscaling on Tensor Cores
  (`vf_vsr`), plus a denoise pass (Low…Ultra) that also works standalone at scale=1
- **AI frame interpolation** — VFX `VideoFrameGeneration` integer multipliers ×2/×3/×4 with
  a hot-switchable quality mode (`vf_vfg`)
- **HDR** — the chain carries **10-bit RGB10A2** end to end (`cuda[x2bgr10]`), so HDR and
  10-bit SDR sources are not crushed to 8 bits before the AI stages; **HDR10 output** is
  enabled automatically for HDR content on an HDR-capable display, and **SDR→HDR10
  upconversion** (VFX TrueHDR, `vf_truehdr`) is available behind an opt-in toggle
- **Frozen legacy backend** — RIFE (TensorRT) interpolation (`vf_rife`) is kept as a
  **source-build option only** (`-Dfruc_backend=rife`): no prebuilt package, no further
  development

## Filter chain

```
demux → decode → [deint] → [vf_hwup] → [vf_eye] → [vf_rot] → [vf_vsr] → [vf_truehdr] → [vf_vfg] → VO (libmpv) → Qt scene graph
                              ↑           ↑          ↑          ↑              ↑             ↑
                       SW→CUDA upload  single eye  rotate   VSR + denoise   TrueHDR        VFG
                                                                          (SDR→HDR10)   (VFX SDK)
```

The order is a cost decision, not a matter of taste — each stage sits where it is billed
**once per source frame** instead of once per output frame:

- **`vf_hwup` first**: software-decoded frames are uploaded to CUDA once, so every
  downstream stage only ever sees hardware frames (no-op when decoding is already hardware).
- **`vf_vsr` before `vf_vfg`**: everything placed *after* interpolation is billed ×2/×3/×4,
  and VSR is the expensive stage (measured: ~8–9 ms for a 1080p→4K frame, ~3 ms for VFG at
  4K, ~5 ms for TrueHDR at 4K). So upscaling runs once per source frame on the small
  picture, and interpolation runs last, on the finished picture. `--fruc-order pre` puts VFG
  in front of VSR — that multiplies the VSR bill by the interpolation factor, which is why
  `post` is the default and `pre` exists only for experiments.
- **`vf_eye` before `vf_vsr`**: single-eye output (see [3D and Single-Eye](3D-and-Single-Eye))
  cuts the eye out first, so the upscaler runs on the per-eye picture instead of the
  side-by-side frame.
- **`vf_rot` before `vf_vsr`**: rotation is a pure pixel shuffle; doing it before upscaling
  means the upscaler sees the final orientation (and its auto factor follows the rotated
  frame).
- **`vf_truehdr` between VSR and VFG**: after VSR so that VSR keeps the SDR input
  distribution it was tuned and validated against, and before VFG so the upconversion also
  runs once per source frame. VFG's 10-bit path then interpolates HDR10 frames natively.
- **`vf_truehdr` is opt-in and self-guarding**: it only runs for SDR content (the filter
  re-checks the frame's transfer function and passes HDR content straight through), and
  only when the toggle is on; with the panel in SDR mode the output stays SDR and mpv
  tone-maps the upconverted frames back down.
- **RIFE builds** use a fixed different order — `hwup → rife → vsr` (interpolation at source
  resolution, then upscaling) with `truehdr` at the tail. Frozen: source build only.

Supporting pieces:

- mpv manages demux, decode, A/V sync, timing, seek and VO.
- `vf_vsr` receives an `mp_image` and outputs an upscaled one; YUV→RGB conversion is a
  self-written NVRTC CUDA kernel (NV12/P010/P016 → RGBA8 or packed RGB10A2, D2D).
- Frontend: Qt 6 + QML (`src/client/`) — `MpvController` (libmpv wrapper),
  `PlayerViewModel` (single source of truth), Vulkan device shared with mpv.
- mpv patch scheme: `third_party/mpv` (pristine 0.41) + `src/mpv` overlay, merged by
  `scripts/build_mpv.sh`.

## Output leg and HDR

`vo=libmpv` renders into a Vulkan swapchain shared with the Qt scene graph. Its format and
the colour-space declaration follow *content × display capability*:

- 8-bit SDR by default;
- **HDR10** (PQ/BT.2020, 10-bit, with the content's static metadata) when HDR content meets
  an HDR-capable display;
- the SDR→HDR10 case when upconversion is on.

Switching happens at runtime (playlist changes, display changes) with no restart. The OSD's
`Output` line reports the **actual** swapchain format, not the intent.

Display capability cannot be read from Qt on Linux (`QRhiSwapChain::hdrInfo()` is a
hardcoded default; `isFormatSupported()` only reports compositor-wide support). The only
reliable source is the surface's preferred description via `wp_color_management_v1`
(`gamut_area` / transfer function); when that is unavailable the player stays in SDR on
purpose.

## Auto-scaled 3D sources

With `scale=auto`, the factor is computed **per eye** — see
[3D and Single-Eye](3D-and-Single-Eye) for the formula and measurements.

## Portrait video and the interpolation quality tier

NVIDIA's VFG engine takes a noticeably slower internal path for **portrait / narrow** inputs
(this is VFX SDK 1.3 behaviour, not code from this project):

| Input (`--fruc-quality medium`) | Engine time per interpolation |
|---|---|
| 1920×1080 landscape | ≈2.5 ms |
| 3840×2160 landscape | ≈3.4 ms |
| 1920×3328 portrait | ≈15–28 ms |

The tier ordering also inverts for portrait: with the same 1920×3328 input, LOW ≈4 ms,
**MEDIUM ≈15–28 ms**, **HIGH ≈5.5 ms**. So for portrait content pick **High** in the quality
popup's "Interpolation quality" row (faster *and* better looking), or Low; **avoid Medium**.
In-player numbers for the same portrait clip (30 fps ×2): MEDIUM ≈15 ms per output frame,
HIGH ≈5.5 ms, LOW ≈4 ms.

Reproducible with `tests/vfg/vfg_timing_probe.c` (a plain NvVFX probe, no player involved).
