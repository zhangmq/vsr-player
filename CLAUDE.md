# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Linux desktop video player that applies real-time AI super-resolution to video playback using the NVIDIA Video Effects SDK C API. The player decodes video via NVDEC, upscales/denoises each frame on the GPU via Tensor Cores, and displays the enhanced output through Vulkan — all in real time during playback.

**This is likely the first open-source Linux player using the VFX SDK directly (not driver-level RTX VSR).** The Linux VFX SDK is still Early Access on NGC. Existing players (mpv, VLC, Moonlight) use driver-level VSR paths that are Windows-only or incomplete on Linux.

## Tech Stack

| Component | Technology | Purpose |
|-----------|-----------|---------|
| Language | C++20 | Application + core library |
| Build | CMake 3.22+ | Cross-platform build |
| Client | Qt 6 (Widgets) | Window, controls, playlist, event loop |
| Decode | FFmpeg libav* C API | NVDEC hardware decode (`av1_cuvid` + `hw_device_ctx`) |
| Super-Res | NvVFX C API (`libnvVFXVideoSuperRes.so`) | AI upscaling on Tensor Cores |
| Render | Vulkan 1.3 | Display pipeline, CUDA-Vulkan interop |
| Window | GLFW (via Qt embedding) or Qt Vulkan | Vulkan surface management |
| Audio | PortAudio | PCM output, master clock for A/V sync |
| CUDA | CUDA Driver API (`libcuda.so`) | GPU context, streams, device pointers |

## Architecture

```
┌─────────────────────────────────┐
│  Qt Client (main thread)        │
│  MainWindow → controls, playlist│
│  VulkanWidget → render embed    │
│  PlayerProxy → command bridge   │
├─────────────────────────────────┤
│  libvsrplayer (worker threads)  │
│  PlayerCore → command dispatch  │
│  Demuxer → Decoder → VSRProc    │
│  → Renderer → Vulkan display    │
│  AudioOutput → PortAudio        │
│  ClockManager → A/V sync        │
└─────────────────────────────────┘
```

**Design:** Single-process, dual-layer (reference: mpv + IINA, VLC + Qt GUI). Qt client and libvsrplayer communicate through a thread-safe command queue — no shared mutable state beyond frame buffers.

### Data Flow

```
Container → Demux → NVDEC → NV12(GPU) → NV12→RGB → float32 RGB(3,H,W) [0,1]
                                                       ↓
                                              VSR Processor (NvVFX)
                                                       ↓
                                        RGBA uint8(GPU) → Vulkan render pass
Audio track → PortAudio → master clock → A/V sync
```

### Module Layout

```
src/
├── client/                    ← Qt client (links libvsrplayer)
│   ├── main.cpp               ← QApplication entry
│   ├── MainWindow.h/cpp       ← QMainWindow
│   ├── PlaylistPanel.h/cpp    ← QListView + model
│   ├── ControlBar.h/cpp       ← play/pause/seek/volume
│   ├── StatusBar.h/cpp        ← fps, resolution, quality info
│   ├── SettingsDialog.h/cpp   ← quality/scale settings
│   ├── VulkanWidget.h/cpp     ← embedded Vulkan render surface
│   └── PlayerProxy.h/cpp      ← libvsrplayer command wrapper + signal relay
│
├── core/                      ← libvsrplayer static library
│   ├── api/
│   │   └── Player.h           ← public C API
│   ├── PlayerCore.h/cpp       ← core controller, event loop
│   ├── CommandQueue.h/cpp      ← thread-safe command queue
│   ├── ClockManager.h/cpp      ← audio-master A/V sync
│   ├── Demuxer.h/cpp           ← FFmpeg demux
│   ├── Decoder.h/cpp           ← NVDEC with hw_device_ctx
│   ├── VSRProcessor.h/cpp      ← NvVFX VideoSuperRes wrapper
│   ├── Renderer.h/cpp          ← Vulkan render pipeline
│   ├── AudioOutput.h/cpp       ← PortAudio wrapper
│   ├── FramePool.h/cpp         ← GPU frame buffer pool
│   └── utils/
│       ├── CUDAContext.h/cpp   ← CUDA device management
│       ├── VulkanContext.h/cpp ← Vulkan instance/device
│       └── NV12ToRGB.h/cpp     ← GPU NV12→RGB conversion
│
└── CMakeLists.txt
```

## Build / Run

```bash
# Prerequisites: NVIDIA driver 570+, Vulkan SDK, Qt 6, FFmpeg dev, PortAudio dev
# NvVFX SDK headers from NGC (EA program) — placed in third_party/nvvfx/

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run
./vsr-player <video_file>
```

## SDK Isolation

VFX SDK .so files (~1.1GB) are bundled with the application. No CUDA Toolkit, TensorRT SDK, or cuDNN installation needed on the user's system. Only `libcuda.so.1` (NVIDIA driver) is required at runtime.

```
build/
├── vsr-player
└── lib/          ← bundled .so files (RPATH: $ORIGIN/lib)
    ├── libnvVFXVideoSuperRes.so
    ├── libVideoFX.so
    ├── libnvinfer.so.10
    ├── libnpp*.so.12  (9 files)
    └── ...
```

## Key Findings from Python Prototype (archive/python-v1/)

- **av1_cuvid without hw_device_ctx → duplicate frames:** `av1_cuvid` requires a CUDA `hw_device_ctx` for proper NVDEC surface management. Using bare `_cuvid` (as PyAV does) produces periodic duplicates (~9/300 frames). The fix is `avctx->hw_device_ctx = av_buffer_ref(hw_device_ctx)` before `avcodec_open2()` — this is available in C API but not exposed by PyAV.
- **VFX SDK bundles all NVIDIA deps:** The pip package includes TensorRT, NPP, cuDNN, NGX in `libs/`. Only `libcuda.so.1` (driver) is external. Portable distribution is feasible.
- **VSR internal CUDA streams:** `torch.cuda.Stream.synchronize()` does NOT cover VSR internal streams. `torch.cuda.synchronize()` (device-level) is required.

## References

- [NVIDIA Video Effects SDK](https://developer.nvidia.com/video-effects-sdk)
- [NVIDIA Maxine Linux VFX SDK (EA) on NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea)
- [NVIDIA VFX Python Samples](https://github.com/NVIDIA-Maxine/nvidia-vfx-python-samples)
- [FFmpeg hw_decode.c — CUDA decode example](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c)
- [FFmpeg cuvid hw_device_ctx patch (c0f17a90)](https://trac.ffmpeg.org/changeset/c0f17a905f3588bf61ba6d86a83c6835d431ed3d/ffmpeg)
