# VSR Player

Real-time AI super-resolution video player for Linux. Uses the NVIDIA Video Effects SDK to apply neural upscaling and denoising during video playback — all on GPU, zero PCIe copies.

## Background

NVIDIA RTX Video Super Resolution (RTX VSR) has been available on Windows for some time, integrated through the driver and supported by browsers and media players. On Linux, however, this driver-level interface is not exposed, and mainstream players (mpv, VLC, etc.) currently have no way to use RTX VSR.

The NVIDIA Video Effects SDK provides access to the same underlying AI models and does offer a Linux version, but it is not a straightforward dependency — it ships as an Early Access SDK with a substantial inference runtime (~1 GB), and there is no established integration path into existing players.

This project calls the Video Effects SDK C API directly from a standalone player. This is not an ideal approach — processing of this kind belongs at the driver or compositor level — and exists only as a workaround until the driver-level VSR interface becomes available on Linux.

## Features

- **AI Super-Resolution** — upscale video 2×/3×/4× in real time via Tensor Cores
- **AI Denoising** — configurable denoise pass (Low to Ultra)
- **NVDEC Hardware Decode** — AV1, H.264, HEVC GPU decoding with zero-copy frames
- **Vulkan Rendering** — CUDA-Vulkan interop, frames never leave GPU memory
- **QML Overlay UI** — semi-transparent controls with auto-hide, playlist, OSD
- **A/V Sync** — audio master clock with real PTS-based synchronization
- **Adaptive Scale** — auto-selects upscale factor based on source resolution and display size
- **Zero System Dependencies** — all VFX libraries (~1.1 GB) bundled; only `libcuda.so.1` required

## Screenshots

![Player UI](docs/images/player-screenshot.jpg)

### VSR Comparison

**Original (720p)**

![Original 720p frame](example/00003_orig.jpg)

**VSR 4× Upscaled**

![VSR 4x upscaled frame](example/00003_vsr.jpg)

## Prerequisites

| Component | Minimum Version |
|-----------|----------------|
| NVIDIA Driver | 570+ |
| Qt 6 | 6.8+ (Quick, QuickControls, Vulkan) |
| FFmpeg | 6.0+ (libavformat, libavcodec, libavutil, libswscale) |
| PortAudio | 19+ |
| Vulkan SDK | 1.3+ (loader + glslc) |
| glslc | (shader compiler, included in Vulkan SDK) |
| C++ Compiler | GCC 13+ or Clang 18+ (C++20) |

## Third-Party SDKs

This project depends on two NVIDIA SDKs. See [third_party/README_en.md](third_party/README_en.md) for details.

| SDK | Component | License | Obtain via |
|-----|-----------|---------|------------|
| CUDA Toolkit | Headers + NVRTC | NVIDIA Proprietary | `sudo pacman -S cuda` (or bundled in release) |
| NvVFX | Headers | MIT | Bundled in repo |
| NvVFX | Runtime (~1.1 GB) | NVIDIA Proprietary | `pip install nvidia-vfx` |

> The NvVFX runtime is **not** included in release packages — NVIDIA's license does not permit redistribution. `install.sh` handles this automatically.

## Quick Start

### Install from Release (recommended)

Download the latest `vsr-player-<ver>-linux-x86_64.tar.gz` from [GitHub Releases](https://github.com/zhangmq/vsr-player/releases).

```bash
tar xzf vsr-player-*.tar.gz
cd vsr-player-*
./install.sh
```

Add to PATH and run:

```bash
export PATH="$PATH:$HOME/vsr-player/bin"
vsr-player /path/to/video.mp4
```

The installer handles system dependency checks, NVIDIA VFX runtime (`pip install nvidia-vfx`), and deployment to `~/vsr-player/`. No root required.

### Build from Source

```bash
# 1. Clone
git clone https://github.com/zhangmq/vsr-player.git
cd vsr-player

# 2. Setup third-party dependencies (see docs/BUILD.md)
#    - CUDA headers/libs at third_party/cuda/ or set CUDA_HOME
#    - NvVFX headers at third_party/nvvfx/include/ (MIT, from GitHub)
#    - NvVFX .so at third_party/nvvfx/lib/ (pip install nvidia-vfx)

# 3. Build (shader compilation included)
make -j$(nproc)

# 4. Run
./build/vsr-player /path/to/video.mp4
```

## CLI Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `--scale` | `off`, `auto`, `2x`, `3x`, `4x` | `auto` | Super-resolution scale |
| `--quality` | `low`, `medium`, `high`, `ultra` | `high` | Upscale quality |
| `--denoise` | `off`, `low`, `medium`, `high`, `ultra` | `off` | Denoise quality (applied at scale=1) |
| `--depth` | integer | `3` | Folder scan depth |
| `--no-hwaccel` | — | — | Disable NVDEC, use software decode |

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / Pause |
| `F` | Toggle Fullscreen |
| `P` | Toggle Playlist |
| `Tab` | Toggle OSD |
| `Esc` | Close playlist / Stop |
| `B` | Previous file |
| `N` | Next file |
| `S` | Screenshot |

## Architecture

```
Qt Client (main thread)                   libvsrplayer (worker threads)
┌──────────────────────────┐          ┌─────────────────────────────────┐
│ QML Overlay              │          │ PlayerCore — command queue +    │
│ ├─ TopBar                │          │              playback state     │
│ ├─ BottomBar             │  Cmd/Evt │ ├─ Demuxer (libavformat)        │
│ ├─ VolumePopup           │◄────────►│ ├─ Decoder (NVDEC hwaccel)      │
│ ├─ QualityPopup          │  bridge  │ ├─ VSRProcessor (NvVFX)         │
│ ├─ SpeedPopup            │          │ ├─ Renderer (Vulkan)            │
│ ├─ PlaylistPanel         │          │ ├─ AudioOutput (PortAudio)      │
│ ├─ ProgressSlider        │          │ ├─ ClockManager (A/V sync)      │
│ ├─ CenterPlayBtn         │          │ └─ NV12ToRGB (CUDA kernel)      │
│ └─ OsdOverlay            │          │                                 │
└──────────────────────────┘          └─────────────────────────────────┘

Data Flow (all GPU, zero PCIe copy):
  Container → Demux → NVDEC → NV12(GPU) → NV12→RGB(GPU) → VSR → RGBA(GPU)
                                                              → CUDA-Vulkan interop
                                                              → Vulkan render pass
                                                              → screen
```

## Directory Structure

```
vsr-player/
├── src/
│   ├── client/                 # Qt client (links libvsrplayer)
│   │   ├── main.cpp            # Entry point, QQuickView + Vulkan init
│   │   ├── PlayerViewModel.*   # QML ↔ Core bridge
│   │   ├── PlaylistEngine.*    # Folder scanner + file list model
│   │   ├── KeyFilter.*         # Keyboard event filter
│   │   ├── QtVulkanContext.*   # RAII Vulkan instance wrapper
│   │   ├── shaders/            # GLSL vertex/fragment shaders
│   │   └── ui/                 # QML components
│   │       ├── overlay.qml     # Main overlay (wiring layer)
│   │       ├── TopBar.qml
│   │       ├── BottomBar.qml
│   │       ├── VolumePopup.qml
│   │       ├── QualityPopup.qml
│   │       ├── SpeedPopup.qml
│   │       ├── PlaylistPanel.qml
│   │       ├── ProgressSlider.qml
│   │       ├── CenterPlayBtn.qml
│   │       ├── OsdOverlay.qml
│   │       └── components/
│   │           └── IconButton.qml
│   └── core/                   # libvsrplayer static library
│       ├── api/Player.h        # Public API (interface, commands, events)
│       ├── PlayerCore.*        # Command queue + state machine
│       ├── Demuxer.*           # FFmpeg avformat wrapper
│       ├── Decoder.*           # NVDEC hwaccel (av1_nvdec, etc.)
│       ├── VSRProcessor.*      # NvVFX VideoSuperRes wrapper
│       ├── Renderer.*          # Vulkan render pipeline
│       ├── AudioOutput.*       # PortAudio wrapper
│       ├── ClockManager.*      # Audio-master A/V sync
│       ├── FramePool.*         # GPU frame buffer manager
│       └── utils/
│           ├── CUDAContext.*   # CUDA device context RAII
│           ├── VulkanContext.* # Vulkan instance/device RAII
│           └── NV12ToRGB.*     # CUDA kernel NV12→RGB
├── tests/                      # Standalone test programs
│   ├── test_decoder.cpp        # hwaccel decoder validation
│   ├── test_pipeline.cpp       # Full decode pipeline test
│   └── test_interop.cpp        # CUDA-Vulkan interop test
├── scripts/
│   └── check-deps.sh           # Third-party dependency checker
├── third_party/                # Bundled dependencies (not in git)
│   ├── cuda/                   # CUDA Toolkit headers + libnvrtc
│   └── nvvfx/                  # NvVFX SDK headers + .so chain
├── docs/
│   ├── ARCHITECTURE.md         # Detailed architecture
│   └── BUILD.md                # Build guide
├── Makefile
├── README.md
├── README_zh.md
└── CLAUDE.md                   # AI assistant context
```

## License

MIT

---

[中文版](README_zh.md)
