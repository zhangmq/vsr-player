# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Linux desktop video player that applies real-time AI super-resolution to video playback using the NVIDIA Video Effects SDK C API. The player decodes video via NVDEC, upscales/denoises each frame on the GPU via Tensor Cores, and displays the enhanced output through Vulkan — all in real time during playback.

**This is likely the first open-source Linux player using the VFX SDK directly (not driver-level RTX VSR).** The Linux VFX SDK is still Early Access on NGC.

## Build / Run

```bash
# Shader compilation (once)
glslc -fshader-stage=vert src/client/shaders/video.vert -o build/video.vert.spv
glslc -fshader-stage=frag src/client/shaders/video.frag -o build/video.frag.spv
glslc -fshader-stage=frag src/client/shaders/nv12.frag -o build/nv12.frag.spv

# Build
make -j$(nproc)

# Run
./build/vsr-player <video_file_or_folder>
```

## Full Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Qt Client (main thread, Qt event loop)                          │
│                                                                  │
│  QQuickView                                                     │
│  ├── TopBar / BottomBar / CenterPlayBtn                          │
│  ├── VolumePopup / QualityPopup / SpeedPopup                     │
│  ├── PlaylistPanel (Drawer + ListView)                           │
│  ├── ProgressSlider / OsdOverlay                                 │
│  └── Auto-hide logic                                             │
│                                                                  │
│  PlayerViewModel — command/event bridge                          │
│  PlaylistEngine — folder scan, file list model                   │
│  KeyFilter — keyboard shortcuts                                  │
│                                                                  │
│  Dependencies: Qt6::Quick, Qt6::QuickControls, libvulkan         │
├──────────────────────────────────────────────────────────────────┤
│  libvsrplayer (worker threads, static library)                   │
│                                                                  │
│  PlayerCore — command queue + playback state machine              │
│  ├── Demuxer        — libavformat (container demux)              │
│  ├── Decoder        — libavcodec + hwaccel framework (NVDEC)     │
│  ├── VSRProcessor   — libnvVFXVideoSuperRes (AI upscale)         │
│  ├── Renderer       — libvulkan + CUDA-Vulkan interop            │
│  ├── AudioOutput    — PortAudio (PCM output)                     │
│  ├── ClockManager   — A/V sync (audio master clock)              │
│  └── NV12ToRGB      — CUDA kernel (GPU format conversion)        │
│                                                                  │
│  Dependencies:                                                    │
│    libavformat libavcodec libavutil libswscale (FFmpeg)          │
│    libportaudio (PortAudio)                                       │
│    libvulkan (Vulkan loader)                                      │
│    libcuda.so.1 (NVIDIA driver — only system-level dep)          │
│    libnvVFXVideoSuperRes.so + full VFX dep chain (bundled)       │
├──────────────────────────────────────────────────────────────────┤
│  Data Flow (all GPU, zero PCIe copy)                              │
│                                                                  │
│  Container ──[FFmpeg]──► NVDEC ──► NV12(GPU)                    │
│    → NV12→RGB(GPU) → VSR → RGBA(GPU)                            │
│    → CUDA-Vulkan interop → Vulkan render pass → screen           │
│                                                                  │
│  Audio ──[FFmpeg]──► PortAudio → master clock → A/V sync         │
└──────────────────────────────────────────────────────────────────┘
```

**Design reference:** mpv + IINA. Single-process, two layers. Client and core communicate through a thread-safe command queue — no shared mutable state beyond frame buffers.

## Module Layout

```
src/
├── client/                       ← Qt client (links libvsrplayer)
│   ├── main.cpp                  ← Entry, QQuickView + Vulkan init
│   ├── PlayerViewModel.h/cpp     ← QML ↔ Core bridge
│   ├── PlaylistEngine.h/cpp      ← Folder scanner + file list
│   ├── KeyFilter.h/cpp           ← Keyboard event filter
│   ├── QtVulkanContext.h/cpp     ← RAII Vulkan instance
│   ├── shaders/                  ← GLSL → SPIR-V
│   └── ui/
│       ├── overlay.qml           ← Main overlay (wiring layer)
│       ├── TopBar.qml
│       ├── BottomBar.qml
│       ├── VolumePopup.qml
│       ├── QualityPopup.qml
│       ├── SpeedPopup.qml
│       ├── PlaylistPanel.qml
│       ├── ProgressSlider.qml
│       ├── CenterPlayBtn.qml
│       ├── OsdOverlay.qml
│       └── components/
│           └── IconButton.qml
│
├── core/                         ← libvsrplayer static library
│   ├── api/Player.h              ← Public API (interface, commands, events)
│   ├── PlayerCore.h/cpp          ← Command queue + state machine
│   ├── CommandQueue.h            ← Thread-safe queue<T>
│   ├── ClockManager.h/cpp        ← Audio-master A/V sync
│   ├── Demuxer.h/cpp             ← FFmpeg avformat wrapper
│   ├── Decoder.h/cpp             ← NVDEC hwaccel (dual-context HW/SW)
│   ├── VSRProcessor.h/cpp        ← NvVFX VideoSuperRes wrapper
│   ├── Renderer.h/cpp            ← Vulkan render pipeline
│   ├── AudioOutput.h/cpp         ← PortAudio wrapper
│   ├── FramePool.h/cpp           ← GPU frame buffer manager
│   └── utils/
│       ├── CUDAContext.h/cpp     ← CUDA device context RAII
│       ├── VulkanContext.h/cpp   ← Vulkan instance/device RAII
│       └── NV12ToRGB.h/cpp       ← CUDA kernel NV12→RGB (NVRTC)
│
└── tests/
    ├── test_decoder.cpp          ← hwaccel decoder validation
    ├── test_pipeline.cpp         ← Full decode pipeline test
    └── test_interop.cpp          ← CUDA-Vulkan interop test
```

### VSRProcessor — Full Dependency Chain

```
libnvVFXVideoSuperRes.so (62K)     ← VSR C API, our direct dependency
  ├── libVideoFX.so (40K)           ← VFX core framework
  ├── libVideoFXLocal.so (5.8M)     ← local processing pipeline
  ├── libNVCVImage.so (4.4M)        ← CUDA image utilities
  ├── libnvngxruntime.so (79K)      ← NGX runtime loader
  │     └── libnvidia-ngx-vsr.so (44M)   ← NGX VSR inference engine
  │           ├── libnvinfer.so.10 (641M)       ← TensorRT
  │           ├── libnvinfer_plugin.so.10 (53M) ← TensorRT plugins
  │           ├── libnvonnxparser.so.10 (4.3M)  ← ONNX model parser
  │           ├── libcudnn.so.9 (123K)          ← cuDNN
  │           └── libnpp*.so.12 ×9 (292M)       ← CUDA NPP
  └── libcuda.so.1                   ← NVIDIA driver (system, only external dep)

Total bundled: ~1.1GB. Shipped in build/lib/, RPATH $ORIGIN/lib.
No CUDA Toolkit, TensorRT SDK, or cuDNN system installation needed.
```

### FFmpeg Roles

| Module | FFmpeg Library | Role |
|--------|---------------|------|
| Demuxer | `libavformat` | Container open, stream discovery, packet read |
| Decoder | `libavcodec` + `libavutil/hwcontext` | NVDEC via hwaccel framework, CUDA device/surface management |
| NV12→RGB (CPU fallback) | `libswscale` | `sws_scale` NV12→RGB24 when CUDA not available |

## SDK Isolation

All VFX SDK dependencies (~1.1GB) are bundled with the application. Runtime dependency: only `libcuda.so.1` (NVIDIA driver).

```
build/
├── vsr-player
└── lib/                          ← bundled .so (RPATH: $ORIGIN/lib)
    ├── libnvVFXVideoSuperRes.so
    ├── libVideoFX.so / libVideoFXLocal.so / libNVCVImage.so
    ├── libnvngxruntime.so / libnvidia-ngx-vsr.so
    ├── libnvinfer.so.10 / libnvinfer_plugin.so.10 / libnvonnxparser.so.10
    ├── libcudnn.so.9
    └── libnpp*.so.12 (9 files)
```

## Key Findings

### Codec selection rules

| Codec | HW path | SW fallback | Never use |
|-------|---------|-------------|-----------|
| AV1 | `av1` + `av1_nvdec` hwaccel | `libdav1d` | `av1_cuvid` |
| H.264 | `h264` + `h264_nvdec` hwaccel | software | `h264_cuvid` |
| HEVC | `hevc` + `hevc_nvdec` hwaccel | software | `hevc_cuvid` |

**Golden rule:** Native decoders + get_format hwaccel. Never `_cuvid` variants (surface management bug → periodic duplicates). Verified: 7202 frames, 0 duplicates.

```c
codec = avcodec_find_decoder_by_name("av1");  // NOT libdav1d, NOT av1_cuvid
avcodec_parameters_to_context(codec_ctx, codecpar);  // extradata required!
codec_ctx->get_format = get_hw_format;          // → AV_PIX_FMT_CUDA
codec_ctx->hw_device_ctx = av_buffer_ref(hw);
avcodec_open2(codec_ctx, codec, nullptr);
// hwaccel (av1_nvdec) activates after first frame — codec_ctx->hwaccel
// is NULL until then.
```

### NVIDIA Vulkan + Wayland

NVIDIA driver supports `VK_KHR_wayland_surface` **if** `nvidia_drm.modeset=1` (kernel cmdline). Check:

```bash
sudo cat /sys/module/nvidia_drm/parameters/modeset  # must be Y
```

Without modesetting, `vkGetPhysicalDeviceSurfaceSupportKHR` returns `VK_FALSE`. Fallback: `QT_QPA_PLATFORM=xcb` (XWayland).

### Other findings

- VFX SDK bundles all deps (TensorRT, NPP, cuDNN). Only `libcuda.so.1` is external.
- VSR internal CUDA streams require device-level sync, not stream-level.
- `hwaccel` field on AVCodecContext is NULL after `avcodec_open2` — set after first frame.
- `avcodec_parameters_to_context()` is REQUIRED for hwaccel init (provides extradata).
- `Item` has no `font` property in any Qt 6 version — use empty string for system default font.
- Qt 6.11 signal handlers with parameters must use explicit `function(param) {}` syntax.

## Environment

- **Qt:** 6.11.1 (CachyOS, pacman)
- **C++:** C++20 (GCC 13+)
- **Build:** Makefile at project root
- **QML:** Qt Quick Controls (plain `import QtQuick.Controls` — project convention, not `.Basic`)

## References

- [NVIDIA Video Effects SDK](https://developer.nvidia.com/video-effects-sdk)
- [NVIDIA Maxine Linux VFX SDK (EA) on NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea)
- [FFmpeg hw_decode.c — CUDA decode example](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c)
