# VSR Player

Real-time AI video processing player for Linux, built directly on the **NVIDIA Video
Effects SDK**: neural super-resolution + denoising, frame interpolation, and a 10-bit
HDR pipeline with HDR10 output and SDR→HDR10 upconversion. Built on **libmpv** (demux,
decode, A/V sync, VO all handled by mpv) with a Qt 6 / QML frontend on a Vulkan device
shared with mpv.

> NVIDIA's driver-level RTX Video Super Resolution has no Linux interface. The Video
> Effects SDK exposes the same models with a Linux runtime, so this project calls its C
> API directly from a player. That is a workaround, not the ideal place for this kind of
> processing — it exists because there is no driver-level path on Linux.

## What it does

- **AI super-resolution + denoise** — real-time 2×/3×/4× (`vf_vsr`), denoise also works at scale=1
- **AI frame interpolation** — VFX `VideoFrameGeneration` ×2/×3/×4 with hot-switchable quality (`vf_vfg`)
- **10-bit / HDR end to end** — RGB10A2 through the whole chain, automatic HDR10 output for HDR content, opt-in SDR→HDR10 upconversion (`vf_truehdr`)
- **3D for XR glasses** — side-by-side / top-and-bottom sources rendered half-per-eye to a 3840×1080-class framebuffer (`StereoController` + composite shader)
- **Single-eye 2D output** — watch one eye of a 3D source as a normal full-frame picture, with VSR applied per eye (`vf_eye`)
- **Blu-ray ISO** — `.iso` behaves like a folder/m3u: mpv expands it into titles, the player shows them with names and durations
- **Everyday player** — NVDEC decode, Vulkan rendering, auto-hide QML UI, playlist, loop modes, speed, screenshots, JSON IPC, standalone `mpv-vsr` CLI

## Recent additions

| Feature | Notes |
|---|---|
| **Single-eye 2D output** | New `vf_eye` filter cuts one eye out of an SBS/TAB frame *before* upscaling, so VSR runs on the per-eye picture. Half-width/height sources are stretched back to natural size so aspect override, rotation and auto-scaling behave like ordinary 2D |
| **3D menu block** | Right-click menu gained a small **3D** group with three independent entries: **Output mode** (Off / 3D glasses / 2D output), **Source layout** (side by side or top and bottom, full or half), **Output option** (left or right eye) |
| **Aspect ratio aligned with mpv** | The bottom-bar popup and the new `A` hotkey cycle the same list mpv ships with: `16:9`, `4:3`, `2.35:1`, `no`. In 3D the value means *per-eye* aspect and is converted before it reaches mpv |
| **Picture processing group** | Deinterlace and rotation are now grouped under one menu header |
| **Blu-ray title naming** | Playlist entries show `Title 6 · 1:18:30` instead of the ISO file name, including after a restart |
| **Playlist snapshot** | The playlist is persisted as an m3u8 file and restored through mpv's own playlist parser, so entry names survive restarts |
| **Startup stays idle** | Launching with no arguments only restores the list — no decode, no engine load, no VRAM use until you press Play |

## Screenshots

![Player UI](https://raw.githubusercontent.com/zhangmq/vsr-player/master/docs/images/player-screenshot.jpg)

**VSR comparison** — original 720p, and the same frame at 4×:

![Original 720p frame](https://raw.githubusercontent.com/zhangmq/vsr-player/master/docs/images/00003_orig.jpg)
![VSR 4x upscaled frame](https://raw.githubusercontent.com/zhangmq/vsr-player/master/docs/images/00003_vsr.jpg)

## Quick start

**Source build** (recommended — the binaries match your system libraries by construction):

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# prepare third_party/ first: see docs/third-party-setup.md
./scripts/build-from-source.sh
```

**Prebuilt tarball** (only `standard`, needs Qt ≥ 6.12, Vulkan loader, NVIDIA driver):

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                 # installs into ~/.local, no sudo
```

Both paths install a **VSR Player** launcher entry and can be used as the default
video/audio handler.

## Requirements

| | Minimum | Notes |
|---|---|---|
| GPU | RTX 20-series | VSR needs RTX 20+; the default interpolation backend needs **Ada/Blackwell** |
| Driver | 570+ | Wayland works out of the box |
| VFX SDK | 1.3 (`nvidia-vfx` 0.2.0.0) | `install.sh` downloads it from NVIDIA's index, never into a Python env |
| Qt | 6.12+ | Build the `full` variant if your distro ships older Qt |

## Playing

```bash
vsr-player video.mkv            # file
vsr-player ~/Videos             # directory → playlist
vsr-player                      # restores the last playlist, loads nothing until Play
```

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `←` / `→` | Seek ∓5 s (`Shift` = ±10 s) |
| `↑` / `↓` | Volume ±5 % |
| `A` / `Shift+A` | Cycle aspect (`16:9` → `4:3` → `2.35:1` → `no`) |
| `P` | Playlist panel |
| `F` | Fullscreen |
| `Tab` | OSD info overlay |
| `S` | Screenshot |
| `N` / `B` | Next / previous file |
| `3D` mode: `J` / `K` / `L` | Layout (SBS/TAB) · eye order · full/half |

The right-click menu holds quality, deinterlace/rotation and the whole 3D block. Full
option list, JSON IPC commands and the standalone `mpv-vsr` CLI: see the wiki.

## Documentation

- **Wiki** — user guides: installation, usage, 3D and single-eye output, Blu-ray, troubleshooting
- [`docs/third-party-setup.md`](https://github.com/zhangmq/vsr-player/blob/master/docs/third-party-setup.md) — preparing `third_party/`
- [`docs/`](https://github.com/zhangmq/vsr-player/tree/master/docs) — engineering notes and measurements (HDR pipeline, VFX SDK, TrueHDR, 3D conversion, …)

## Status & scope

The video-processing side is feature-complete for its intended scope. Everything else —
window chrome, control layout, hover behaviour, OSD wording — reflects the author's
preference rather than a specification and may change at any time; this README is not a
roadmap.

Development happens on a single GPU generation and a small set of test files, so rough
edges are expected: crashes, artifacts or hangs that were never seen here can still
happen on other combinations. Bug reports and pull requests are welcome. The repository
is deliberately agent-friendly — an AI coding agent can read the pipeline, the mpv patch
overlay and the design records in `docs/` to locate issues quickly.

## License

GPLv2+. The project compiles a patched mpv and links against libmpv, so distributed
binaries are derivative works of mpv and are distributed under the same license. The
NVIDIA Video Effects SDK runtime is NVIDIA proprietary software and is not covered by
this project's license.
