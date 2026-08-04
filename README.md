# VSR Player

Real-time AI super-resolution video player for Linux. Uses the NVIDIA Video Effects SDK to apply neural upscaling and denoising during video playback.

Built on **libmpv** — demux, decode, A/V sync, timing, and VO rendering are handled by mpv. VSR is injected as a custom mpv video filter (`vf_vsr`), which upscales via CUDA + the Video Effects SDK and feeds the result back into the mpv pipeline. The frontend is a Qt 6 + QML client (Vulkan, shared device with mpv).

## Background

NVIDIA RTX Video Super Resolution (RTX VSR) has been available on Windows for some time, integrated through the driver and supported by browsers and media players. On Linux, however, this driver-level interface is not exposed, and mainstream players (mpv, VLC, etc.) currently have no way to use RTX VSR.

The NVIDIA Video Effects SDK provides access to the same underlying AI models and does offer a Linux version, but it is not a straightforward dependency — it ships as an Early Access SDK with a substantial inference runtime (~1 GB), and there is no established integration path into existing players.

This project calls the Video Effects SDK C API directly from a player. This is not an ideal approach — processing of this kind belongs at the driver or compositor level — and exists only as a workaround until the driver-level VSR interface becomes available on Linux.

## Features

- **AI Super-Resolution** — real-time 2×/3×/4× upscaling via Tensor Cores (mpv video filter `vf_vsr`)
- **AI Denoising** — configurable denoise pass (Low to Ultra), works standalone at scale=1
- **NVDEC Hardware Decode** — AV1, H.264, HEVC GPU decoding (software fallback)
- **Vulkan Rendering** — CUDA-Vulkan shared device, mpv renders into the Qt scene graph
- **QML Overlay UI** — auto-hide controls (bottom hover zone driven), playlist with virtualized list, OSD info panel
- **Adaptive Scale** — auto-selects upscale factor based on viewport size
- **Playlist & Playback** — directory loading, loop modes, speed control, A/V sync handled by mpv
- **Remote Control** — JSON IPC over Unix socket; standalone `mpv-vsr` CLI and wrapper script

## Screenshots

![Player UI](docs/images/player-screenshot.jpg)

### VSR Comparison

**Original (720p)**

![Original 720p frame](docs/images/00003_orig.jpg)

**VSR 4× Upscaled**

![VSR 4x upscaled frame](docs/images/00003_vsr.jpg)

## Architecture

```
demux → decode → [vf_vsr] → VO (libmpv) → Qt scene graph
                   ↑
              VFX SDK + CUDA
```

- mpv manages: demux, decode, A/V sync, timing, seek, VO
- `vf_vsr` (patch overlay `src/mpv/video/filter/vf_vsr.c`): receives `mp_image`, upscales via CUDA+VFX SDK, outputs upscaled `mp_image`
- Frontend: Qt 6 + QML (`src/client/`) — MpvController (libmpv wrapper), PlayerViewModel (single source of truth), Vulkan shared device
- mpv patch scheme: `third_party/mpv` (pristine 0.41) + `src/mpv` overlay, merged by `scripts/build_mpv.sh`

## Prerequisites

| Component | Requirement |
|-----------|-------------|
| GPU | NVIDIA RTX 20-series or newer |
| Driver | 570+ (with CUDA; `nvidia_drm.modeset=1` for Wayland) |
| Qt | 6.8+ (Quick, QuickControls, Vulkan) |
| C++ Compiler | GCC 13+ (C++20) |
| Build | meson, ninja, CUDA Toolkit (`/opt/cuda`) |

Third-party SDKs (NvVFX headers/runtime, MDI icon font, mpv source) are **not** bundled in the repo — see [docs/third-party-setup.md](docs/third-party-setup.md) to prepare `third_party/`.

## Building

```bash
./scripts/build_mpv.sh          # merge src/mpv overlay into third_party/mpv and build libmpv + vf_vsr
ninja -C build                  # build the Qt client
./build/src/client/vsr-player <video-or-directory>
```

## Usage

- Play a file or directory (a directory loads all playable files into the playlist)
- Quality control: bottom bar `Quality` popup — scale off/auto/2×/3×/4×, VSR quality, denoise
- Playlist panel: `P`; auto-hide UI driven by the bottom hover zone (mouse leaves → UI hides)
- OSD: `Tab` toggles the mpv-rendered info overlay (source, output, render, decoder, GPU…)

### CLI Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--scale` | `off`, `auto`, `2`, `3`, `4` | `auto` | Super-resolution scale |
| `--quality` | `low`, `medium`, `high`, `ultra` | `high` | Upscale quality |
| `--denoise` | `off`, `low`, `medium`, `high`, `ultra` | `off` | Denoise quality (applied at scale=1) |
| `--no-hwaccel` | — | — | Disable NVDEC, use software decode |
| `--lang` | e.g. `en`, `zh_CN` | system locale | UI language |
| `--benchmark` | — | — | Headless throughput measurement (no UI, `all=no` logging) |
| `--no-vsync` | — | — | Disable FIFO present |
| `--no-rpc` | — | — | Disable JSON IPC server |

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / Pause |
| `←` / `→` | Seek ±5s (`Shift` + arrow = ±10s) |
| `↑` / `↓` | Volume ±5% |
| `S` | Screenshot |
| `Tab` | Toggle OSD |
| `N` / `B` | Next / Previous file |
| `[` / `]` / `\` | Speed 0.5× / 2× / 1× |
| `P` | Toggle Playlist |
| `F` | Toggle Fullscreen |
| `Esc` | Exit fullscreen / close playlist |

## Remote Control

JSON IPC over Unix socket (`/tmp/vsr-player.sock`):

```bash
printf '{"command":["play"]}\n' | socat - UNIX-CONNECT:/tmp/vsr-player.sock
```

Commands: `play`, `pause`, `stop`, `seek`, `loadfile`, `set-vsr`, `get-vsr`, `quit`, plus raw `command` passthrough. A standalone CLI (`scripts/install_mpv_local.sh`) installs `mpv-vsr` — the patched mpv binary with `--vf=vsr` support.

## Standalone mpv-vsr CLI

The patched mpv binary (with the `vf_vsr` filter) is also built and shipped standalone,
usable as a plain mpv replacement:

```bash
./scripts/install_mpv_local.sh     # → ~/.local/bin/mpv-vsr + VFX libs in ~/.local/lib/vsr-player/
mpv-vsr --vf=vsr:scale=2 video.mkv
```

Filter options:

| Option | Values | Description |
|--------|--------|-------------|
| `scale` | `off`, `auto`, `2`, `3`, `4`, ratio (e.g. `4/3`) | Upscale factor; `auto` picks by viewport |
| `quality` | `low`, `medium`, `high`, `ultra` | VSR inference quality |
| `denoise` | `off`, `low`, `medium`, `high`, `ultra` | Denoise pass (works at scale=1) |

Example: `mpv-vsr --vf=vsr:scale=auto,quality=ultra --hwdec=auto video.mkv`

`mpv-vsr-wrapper.py` (browser integration, ff2mpv-style): exports Chrome cookies,
extracts playlists via yt-dlp (YouTube/Bilibili/Niconico), and launches mpv-vsr.

## License

VSR Player is licensed under the GNU General Public License version 2 or later (GPLv2+).

See [LICENSE](LICENSE) for the full license text.

The project builds on [mpv](https://github.com/mpv-player/mpv) (GPLv2+): it compiles a
patched mpv (custom `vf_vsr` filter) and links the frontend against libmpv — the
distributed binaries are therefore derivative works of mpv, and the whole project is
distributed under GPLv2+. The Qt/QML frontend code itself is developed independently.

The NVIDIA Video Effects SDK runtime libraries loaded at runtime are NVIDIA proprietary
software and are not subject to the GPL license of this project.

---

[中文版](README_zh.md)
