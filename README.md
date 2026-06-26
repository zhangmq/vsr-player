# VSR Player

Real-time AI super-resolution video player for Linux. Applies NVIDIA Video Effects SDK (NvVFX) neural upscaling and denoising to video playback — all on GPU, zero PCIe copies.

> Likely the first open-source Linux player using the NvVFX SDK directly (not driver-level RTX VSR). The Linux VFX SDK is still Early Access on NGC.

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

<!-- TODO: add screenshots -->

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

## Quick Start

```bash
# 1. Clone
git clone <repo-url>
cd vsr-player

# 2. Setup third-party dependencies (see docs/BUILD.md for details)
#    - CUDA headers from /opt/cuda
#    - NvVFX headers from GitHub
#    - NvVFX .so files from NGC or pip package

# 3. Compile shaders (one-time)
glslc -fshader-stage=vert src/client/shaders/video.vert -o build/video.vert.spv
glslc -fshader-stage=frag src/client/shaders/video.frag -o build/video.frag.spv
glslc -fshader-stage=frag src/client/shaders/nv12.frag -o build/nv12.frag.spv

# 4. Build
make -j$(nproc)

# 5. Run
./build/vsr-player /path/to/video.mp4
# or open a folder
./build/vsr-player /path/to/videos/
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
