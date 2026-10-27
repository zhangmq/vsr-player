# VSR Player

Real-time AI video processing player for Linux, built directly on the **NVIDIA Video
Effects SDK**: neural super-resolution + denoising, frame interpolation, and a 10-bit HDR
pipeline with HDR10 output and SDR→HDR10 upconversion. Built on **libmpv** (demux, decode,
A/V sync, VO all handled by mpv) with a Qt 6 / QML frontend on a Vulkan device shared with
mpv.

> NVIDIA's driver-level RTX Video Super Resolution has no Linux interface. The Video Effects
> SDK exposes the same models with a Linux runtime, so this project calls its C API directly
> from a player. That is a workaround, not the ideal place for this kind of processing — it
> exists because there is no driver-level path on Linux.

## What it does

- **AI super-resolution + denoise** — real-time 2×/3×/4× (`vf_vsr`); denoise also works at scale=1
- **AI frame interpolation** — VFX `VideoFrameGeneration` ×2/×3/×4, quality hot-switchable (`vf_vfg`)
- **10-bit / HDR end to end** — RGB10A2 through the whole chain, automatic HDR10 output for HDR content, opt-in SDR→HDR10 upconversion (`vf_truehdr`)
- **3D on XR glasses** — play 3D videos (side-by-side or top-and-bottom) on XR glasses in 3D mode: fullscreen picture on the glasses, controls stay on the desktop
- **2D output** — watch one eye of a 3D source as a normal full-frame picture, with VSR applied per eye (`vf_eye`)
- **Blu-ray ISO** — `.iso` behaves like a folder or m3u: titles are listed with names and durations, nothing auto-plays
- **Everyday player** — NVDEC decode, Vulkan rendering, auto-hide QML UI, playlist, loop modes, speed, screenshots, JSON IPC, standalone `mpv-vsr` CLI

## Recent additions

| Feature | Summary |
|---|---|
| **2D output** | New `vf_eye` filter extracts one eye *before* upscaling, so VSR runs on the per-eye picture. Half-width/height sources are stretched to natural size inside the filter, so aspect override, rotation and auto-scaling behave like ordinary 2D |
| **3D menu block** | The right-click menu gained a **3D** group with three independent entries: **Output mode** (Off / 3D glasses / 2D output), **Source layout** (side by side or top and bottom, full or half), **Output option** (left or right eye) |
| **Aspect ratio aligned with mpv** | The bottom-bar popup and a new `A` hotkey cycle mpv's own list — `16:9`, `4:3`, `2.35:1`, `no`. In 3D the value means *per-eye* aspect and is converted before reaching mpv |
| **Picture processing group** | Deinterlace and rotation are grouped under one menu header |
| **Blu-ray title naming** | Playlist rows read `Title 6 · 1:18:30` instead of the ISO file name — including after a restart |
| **Playlist snapshot** | The playlist is persisted as an m3u8 file and restored through mpv's own playlist parser, so entry names survive restarts |
| **Startup stays idle** | Launching with no arguments only restores the list: no decode, no engine load, no VRAM use until Play |

## Screenshots

![Player UI](docs/images/player-screenshot.jpg)

**VSR comparison** — original 720p and the same frame at 4×:

![Original 720p frame](docs/images/00003_orig.jpg)
![VSR 4x upscaled frame](docs/images/00003_vsr.jpg)

## Quick start

**Source build** (recommended — the binaries match your system libraries by construction):

```bash
git clone https://github.com/zhangmq/vsr-player && cd vsr-player
# prepare third_party/ first: see docs/third-party-setup.md
./scripts/build-from-source.sh
```

**Prebuilt tarball** — `standard` is a **thin ~6 MB package**: it ships the players, libmpv
and the Wayland helper modules; FFmpeg, the media stack, CUDA and **Qt ≥ 6.12** come from
your system (VFX is downloaded at install time):

```bash
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                 # installs into ~/.local, no sudo
```

Both install a **VSR Player** launcher entry and can be used as the default video/audio
handler. Packaging variants, manual builds and version compatibility:
[wiki: Installation](https://github.com/zhangmq/vsr-player/wiki/Installation).

## Requirements

| | Minimum | Notes |
|---|---|---|
| GPU | RTX 20-series | VSR needs RTX 20+; the default interpolation backend needs **Ada/Blackwell** (Ampere+ for the frozen RIFE source build) |
| Driver | 570+ | Wayland works out of the box |
| VFX SDK | 1.3 (`nvidia-vfx` 0.2.0.0) | `install.sh` fetches it from NVIDIA's index, never into a Python environment |
| Qt | 6.12+ | Otherwise build the `full` variant yourself |

## Playing

```bash
vsr-player video.mkv            # file
vsr-player ~/Videos             # directory → playlist
vsr-player                      # restore the last playlist, load nothing until Play
```

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `←` / `→` | Seek ∓5 s (`Shift` = ±10 s) |
| `↑` / `↓` | Volume ±5 % |
| `A` / `Shift+A` | Cycle aspect ratio |
| `P` / `F` / `Tab` / `S` | Playlist · fullscreen · OSD · screenshot |
| `N` / `B` | Next / previous entry |
| `J` / `K` / `L` | 3D or single-eye mode: layout · eye order · full/half |

The right-click menu holds quality, **Picture processing** (deinterlace, rotation) and the
whole 3D block. Every option, the JSON IPC commands and the standalone `mpv-vsr` CLI:
[wiki: Usage](https://github.com/zhangmq/vsr-player/wiki/Usage).

## Documentation

- **Wiki** — [Installation](https://github.com/zhangmq/vsr-player/wiki/Installation) ·
  [Usage](https://github.com/zhangmq/vsr-player/wiki/Usage) ·
  [Video Processing](https://github.com/zhangmq/vsr-player/wiki/Video-Processing) ·
  [3D and Single-Eye](https://github.com/zhangmq/vsr-player/wiki/3D-and-Single-Eye) ·
  [Blu-ray and Playlists](https://github.com/zhangmq/vsr-player/wiki/Blu-ray-and-Playlists) ·
  [Troubleshooting](https://github.com/zhangmq/vsr-player/wiki/Troubleshooting) ·
  [Development](https://github.com/zhangmq/vsr-player/wiki/Development)
- [`docs/`](docs) — engineering notes and measurements (HDR pipeline, VFX SDK, TrueHDR, 3D conversion, …)
- [`docs/third-party-setup.md`](docs/third-party-setup.md) — preparing `third_party/` for source builds

## Status & scope

The video-processing side is feature-complete for its intended scope. Everything else —
window chrome, control layout, hover behaviour, OSD wording — reflects the author's
preference rather than a specification and may change at any time; this README is not a
roadmap.

Development happens on a single GPU generation and a small set of test files, so rough edges
are expected: crashes, artifacts or hangs that were never seen here can still happen on other
combinations. Bug reports and pull requests are welcome. The repository is deliberately
agent-friendly — an AI coding agent can read the pipeline, the mpv patch overlay and the
design records in `docs/` to locate issues quickly.

## License

GPLv2+. The project compiles a patched mpv and links against libmpv, so distributed binaries
are derivative works of mpv and are distributed under the same license. The NVIDIA Video
Effects SDK runtime is NVIDIA proprietary software and is not covered by this project's
license. See [LICENSE](LICENSE).

---

[中文版](README_zh.md)
