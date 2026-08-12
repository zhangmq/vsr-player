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
- **AI Frame Interpolation (FRUC)** — RIFE-based motion interpolation to 30/40/60 fps or any target (mpv video filter `vf_rife`, TensorRT, runs before VSR at source resolution)
- **NVDEC Hardware Decode** — AV1, H.264, HEVC GPU decoding (software fallback; SW frames auto-uploaded via `vf_hwup`)
- **Vulkan Rendering** — CUDA-Vulkan shared device, mpv renders into the Qt scene graph
- **QML Overlay UI** — auto-hide controls (bottom hover zone driven), playlist with virtualized list, OSD info panel
- **Adaptive Scale** — auto-selects upscale factor based on viewport size
- **Playlist & Playback** — directory loading, loop modes, speed control, A/V sync handled by mpv
- **Remote Control** — JSON IPC over Unix socket; standalone `mpv-vsr` CLI and wrapper script

## Testing Status

This project is developed on limited hardware — a single GPU generation and a small set of test files. Not every GPU / driver / container combination can be covered, so expect rough edges: you may hit crashes, visual artifacts, or hangs that have never been seen here.

If something breaks, an effective path is to let an AI coding agent help you debug. This project itself is developed with AI agents (Claude Code): the code, the mpv patch overlay, and the design records in `docs/` and commit history are all in place for an agent to understand the pipeline and locate issues quickly. Bug reports and pull requests are welcome either way.

## Screenshots

![Player UI](docs/images/player-screenshot.jpg)

### VSR Comparison

**Original (720p)**

![Original 720p frame](docs/images/00003_orig.jpg)

**VSR 4× Upscaled**

![VSR 4x upscaled frame](docs/images/00003_vsr.jpg)

## Architecture

```
demux → decode → [vf_hwup] → [vf_rife] → [vf_vsr] → VO (libmpv) → Qt scene graph
                ↑            ↑               ↑
           SW→CUDA upload   RIFE (TRT)    VFX SDK + CUDA
```

- mpv manages: demux, decode, A/V sync, timing, seek, VO
- `vf_hwup`: SW frames (software decode) uploaded to CUDA so downstream filters always see hardware frames
- `vf_rife` (patch overlay `src/mpv/video/filter/vf_rife.c`): RIFE frame interpolation (TensorRT), runs at source resolution before upscaling
- `vf_vsr` (patch overlay `src/mpv/video/filter/vf_vsr.c`): receives `mp_image`, upscales via CUDA+VFX SDK, outputs upscaled `mp_image`
- Frontend: Qt 6 + QML (`src/client/`) — MpvController (libmpv wrapper), PlayerViewModel (single source of truth), Vulkan shared device
- mpv patch scheme: `third_party/mpv` (pristine 0.41) + `src/mpv` overlay, merged by `scripts/build_mpv.sh`

## Prerequisites

| Component | Requirement |
|-----------|-------------|
| GPU | NVIDIA RTX 20-series or newer (FRUC needs Ampere+ with FP16 Tensor Cores) |
| Driver | 570+ (with CUDA; `nvidia_drm.modeset=1` for Wayland) |
| Qt | 6.11+ (Quick, QuickControls, Vulkan) |
| C++ Compiler | GCC 13+ (C++20) |
| Build | meson, ninja, CUDA Toolkit (`/opt/cuda`), TensorRT (system `trtexec`, for engine builds) |

Third-party SDKs (NvVFX headers/runtime, MDI icon font, mpv source) are **not** bundled in the repo — see [docs/third-party-setup.md](docs/third-party-setup.md) to prepare `third_party/`.

## Building from Source

1. **Prepare `third_party/`** — NvVFX SDK headers/runtime, MDI icon font, mpv source, CUDA 12 archive, RIFE ONNX asset: follow [docs/third-party-setup.md](docs/third-party-setup.md).
2. **Build and run:**

```bash
./scripts/build_mpv.sh          # merge src/mpv overlay → third_party/mpv, build libmpv + filters
ninja -C build                  # build the Qt client
./build/src/client/vsr-player <video-or-directory>
```

**Notes / gotchas:**

- **mpv patch scheme**: `third_party/mpv` is the pristine base; `src/mpv/` is the overlay (mirrors the mpv tree, only modified files). After editing anything under `src/mpv/`, you **must** re-run `./scripts/build_mpv.sh` — `build/mpv` is a *merged copy*, and running `ninja` on it alone silently keeps the stale copy (a known footgun; a full rebuild surfaces it).
- **`build_mpv.sh` output is grep-filtered** — a failed compile can be hidden. Confirm the build finished by checking for the final "Done" line.
- **Don't `cd build` and then use `./build/...`** — relative paths break. Stay at the repo root or use absolute paths.
- **After system library upgrades** (FFmpeg, Qt, TensorRT — pacman/apt), rebuild everything (`./scripts/build_mpv.sh` + `ninja -C build`): stale binaries link the old sonames and fail to start.
- **RIFE engine**: built with the system `trtexec` via `bash tests/fruc/build_rife_full_engine.sh` (dynamic-shape FP16, `--hardware-compat on` for cross-architecture). Requires the RIFE ONNX asset in `third_party/rife/`.
- **Development install**: `./scripts/install.sh` works from a repo checkout too (dev mode — picks up the build tree automatically, no tarball needed).
- **Distributable build**: `./scripts/build_release.sh` — release build + `$ORIGIN`-relative RPATH + dependency collection + tarball. `build_mpv.sh`/client builds accept `MPV_BUILD_DIR`, `BUILDTYPE`, `DIST_RPATH` env overrides (used by the release scripts).

## Distribution (release tarball)

```bash
./scripts/build_release.sh      # → build/vsr-player-<ver>-linux-x86_64.tar.xz (~316 MB)
tar -xJf vsr-player-<ver>-linux-x86_64.tar.xz
./install.sh                    # installs to ~/.local (bin + lib/vsr-player), no sudo
```

- Bundled: libmpv + ffmpeg ×7 + CUDA runtime + TensorRT 11 + RIFE engine (ampere+, one engine for 30/40/50-series GPUs) + fonts/translations/licenses
- **Not bundled**: VFX SDK (~1.1 GB) — NVIDIA SLA restricts redistribution; `install.sh` offers to fetch it from the official PyPI `nvidia-vfx` wheel (curl download + extract, no pip install, no system changes), or you can place it in `~/.local/lib/vsr-player/` manually
- GUI runtime needs Qt ≥ 6.11 from the system; the only hard external dependency is the NVIDIA driver (`libcuda.so.1`)

## Version Compatibility

| Component | Binding | If mismatched |
|-----------|---------|---------------|
| **VFX SDK ↔ driver** | The latest PyPI wheel may require a newer driver; an old driver + new VFX → VSR fails to load | Upgrade the driver, or pin an older VFX version (below) |
| **RIFE engine ↔ TensorRT** | Engine files embed the exact TRT version that built them — deserialization fails on version mismatch (verified in both directions) | Rebuild the engine with your system TRT (`bash tests/fruc/build_rife_full_engine.sh`), or install a matching TRT. Tarball users: engine + bundled TRT ship together and are self-consistent. The VFX SDK's own TRT 10 libs coexist with RIFE's TRT 11 in one process (RTLD_LOCAL isolation) — nothing to do |
| **Qt** | Hard requirement ≥ 6.11 (QML/QuickControls features used) | No fallback — upgrade the system Qt |
| **GPU** | VSR needs RTX 20+; FRUC needs Ampere+ (FP16 Tensor Cores) | Older GPUs: VSR works, interpolation degrades to passthrough |
| **Driver** | 570+ for VFX; `nvidia_drm.modeset=1` for Wayland | Upgrade, or pin an older VFX wheel |

**Pinning a VFX SDK version** — `install.sh` always fetches the **latest** `nvidia-vfx` wheel from PyPI. If the default doesn't work with your setup (e.g. driver too old), you are not forced to use it:

```bash
# 1. list available versions
pip index versions nvidia-vfx        # or: pypi.org/project/nvidia-vfx/#files

# 2. download a specific version's wheel (pip download only fetches; no install)
pip download nvidia-vfx==<version> --no-deps -d /tmp/vfx

# 3. extract its libs into the app's lib dir
unzip -o /tmp/vfx/nvidia_vfx-<version>*.whl "nvvfx/libs/*" -d /tmp/vfx
cp /tmp/vfx/nvvfx/libs/*.so* ~/.local/lib/vsr-player/
```

Note: `vsr_proc.c` dlopens the VFX libs by their **unversioned** names (`libnppc.so`, `libcudnn.so`, `libnvidia-ngx-vsr.so`, …), but the wheel only ships versioned files (`.so.12`, `.so.9`, …). The automatic download path in `install.sh` creates the missing unversioned symlinks; if you placed the files manually, create them yourself or the VFX load chain breaks (VSR silently passes through):

```bash
cd ~/.local/lib/vsr-player/
for t in libnppc libnppial libnppicc libnppidei libnppig libnppif \
         libnppim libnppist libnppitc libcudnn libnvidia-ngx-vsr; do
  for s in "$t".so.*; do [ -e "$s" ] && ln -sf "$s" "$t.so" && break; done
done
```

`install.sh` never forces a version onto your system — everything lives in `~/.local/lib/vsr-player/`, and replacing the VFX files there is the supported way to switch.

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
| `--fruc` | `off`, `30`, `40`, `60`, `2`, `3`, `4` | persisted | Frame interpolation: target fps (30/40/60) or ×multiplier (2/3/4, benchmark mode) |
| `--no-hwaccel` | — | — | Disable NVDEC, use software decode |
| `--lang` | e.g. `en`, `zh_CN` | system locale | UI language |
| `--benchmark` | — | — | Headless throughput measurement (no UI, `all=no` logging) |
| `--vsync` | — | off | FIFO present (non-blocking by default; Wayland has no tearing) |
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

The patched mpv binary (with `vf_vsr` + `vf_rife` + `vf_hwup` filters) is also built and shipped standalone,
usable as a plain mpv replacement:

```bash
mpv-vsr --hwdec=auto --vf=hwup,rife:fps=60,vsr:scale=2 video.mkv
```

Filter chain (left to right): `hwup` (SW→CUDA upload, enables soft-decode path) → `rife` (interpolation, source resolution) → `vsr` (upscaling). Use `--hwdec=nvdec` for hardware decode (then `hwup` is a no-op passthrough).

Filter options:

| Option | Values | Description |
|--------|--------|-------------|
| `scale` (vsr) | `off`, `auto`, `2`, `3`, `4`, ratio (e.g. `4/3`) | Upscale factor; `auto` picks by viewport |
| `quality` (vsr) | `low`, `medium`, `high`, `ultra` | VSR inference quality |
| `denoise` (vsr) | `off`, `low`, `medium`, `high`, `ultra` | Denoise pass (works at scale=1) |
| `fps` (rife) | `off`, `auto`, integer 1..120 | Interpolation target fps; `auto` = 2×source |
| `scale` (rife) | `off`, `2`, `3`, `4` | Benchmark multiplier (no adaptive passthrough) |
| `adaptive` (rife) | `yes`, `no` | Cost-based passthrough when interpolation is too slow |

Example: `mpv-vsr --hwdec=nvdec --vf=rife:fps=60,vsr:scale=auto,quality=ultra video.mkv`

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
