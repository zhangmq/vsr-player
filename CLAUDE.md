# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Linux desktop video player that applies real-time AI super-resolution to video playback using the NVIDIA Video Effects SDK C API. The player decodes video via NVDEC, upscales/denoises each frame on the GPU via Tensor Cores, and displays the enhanced output through Vulkan — all in real time during playback.

**This is likely the first open-source Linux player using the VFX SDK directly (not driver-level RTX VSR).** The Linux VFX SDK is still Early Access on NGC. Existing players (mpv, VLC, Moonlight) use driver-level VSR paths that are Windows-only or incomplete on Linux.

## Full Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Qt Client (main thread, Qt event loop)                          │
│                                                                  │
│  QMainWindow                                                     │
│  ├── PlaylistPanel (QListView)                                   │
│  ├── VulkanWidget (VkSurfaceKHR embedded in QWidget layout)      │
│  ├── ControlBar (play/pause/stop/seek/volume)                    │
│  └── StatusBar (resolution/fps/quality)                          │
│                                                                  │
│  PlayerProxy — command/event bridge                              │
│                                                                  │
│  Dependencies: Qt6::Widgets, libvulkan.so                        │
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

**Design reference:** mpv + IINA, VLC + Qt GUI. Single-process, two layers. Qt client and libvsrplayer communicate through a thread-safe command queue — no shared mutable state beyond frame buffers.

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
  │           └── libnpp*.so.12 ×9 (292M)       ← CUDA NPP image processing
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

FFmpeg is the implementation engine for Demuxer and Decoder — not a standalone layer. `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA)` creates the CUDA context used throughout the pipeline.

### Module Layout

```
src/
├── client/                       ← Qt client (links libvsrplayer)
│   ├── main.cpp                  ← QApplication entry
│   ├── MainWindow.h/cpp          ← QMainWindow, layout
│   ├── PlaylistPanel.h/cpp       ← QListView + model
│   ├── ControlBar.h/cpp          ← play/pause/seek/volume
│   ├── StatusBar.h/cpp           ← fps, resolution, quality
│   ├── SettingsDialog.h/cpp      ← quality/scale settings
│   ├── VulkanWidget.h/cpp        ← VkSurfaceKHR in QWidget
│   ├── PlayerProxy.h/cpp         ← command/event bridge
│   └── shaders/                  ← GLSL → SPIR-V at build time
│
├── core/                         ← libvsrplayer static library
│   ├── api/
│   │   └── Player.h              ← public API (Player interface)
│   ├── PlayerCore.h/cpp          ← command queue + state machine
│   ├── CommandQueue.h            ← thread-safe queue<T>
│   ├── ClockManager.h/cpp        ← audio-master A/V sync
│   ├── Demuxer.h/cpp             ← FFmpeg avformat wrapper
│   ├── Decoder.h/cpp             ← NVDEC hwaccel (av1_nvdec etc.)
│   ├── VSRProcessor.h/cpp        ← NvVFX VideoSuperRes wrapper
│   ├── Renderer.h/cpp            ← Vulkan render pipeline
│   ├── AudioOutput.h/cpp         ← PortAudio wrapper
│   ├── FramePool.h/cpp           ← GPU frame buffer manager
│   └── utils/
│       ├── CUDAContext.h/cpp     ← CUDA device context RAII
│       ├── VulkanContext.h/cpp   ← Vulkan instance/device RAII
│       └── NV12ToRGB.h/cpp       ← CUDA kernel NV12→RGB
│
├── CMakeLists.txt
└── tests/
    ├── test_decoder.cpp           ← hwaccel decoder validation
    └── test_pipeline.cpp          ← full decode pipeline test
```

## Build / Run

```bash
# Prerequisites: NVIDIA driver 570+, Vulkan SDK, Qt 6, FFmpeg dev, PortAudio dev
# NvVFX SDK headers from NGC (EA program) → third_party/nvvfx/include/
# NvVFX .so files from pip package → build/lib/

# Shader compilation (once)
glslc -fshader-stage=vert src/client/shaders/video.vert -o build/video.vert.spv
glslc -fshader-stage=frag src/client/shaders/video.frag -o build/video.frag.spv

# Full build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Quick test builds (no CMake needed)
mkdir -p build/tests
g++ -std=c++20 -O2 -Wall -o build/tests/test_decoder \
    tests/test_decoder.cpp \
    $(pkg-config --cflags --libs libavcodec libavformat libavutil) -lcuda
g++ -std=c++20 -O2 -Wall -o build/tests/test_pipeline \
    tests/test_pipeline.cpp src/core/Demuxer.cpp src/core/Decoder.cpp \
    $(pkg-config --cflags --libs libavcodec libavformat libavutil libswscale) \
    -lcuda -Isrc/core -Isrc/core/api

# Run tests
./build/tests/test_decoder input/catlove_720p.webm
./build/tests/test_pipeline input/catlove_720p.webm [frames_to_save]

# Run player (when implemented)
./build/vsr-player <video_file>
```

## SDK Isolation

All VFX SDK dependencies (~1.1GB) are bundled with the application. No CUDA Toolkit, TensorRT SDK, or cuDNN installation needed on the user's system.

```
build/
├── vsr-player
└── lib/                          ← bundled .so (RPATH: $ORIGIN/lib)
    ├── libnvVFXVideoSuperRes.so
    ├── libVideoFX.so
    ├── libVideoFXLocal.so
    ├── libNVCVImage.so
    ├── libnvngxruntime.so
    ├── libnvidia-ngx-vsr.so
    ├── libnvinfer.so.10
    ├── libnvinfer_plugin.so.10
    ├── libnvonnxparser.so.10
    ├── libcudnn.so.9
    └── libnpp*.so.12 (9 files)
```

Runtime dependency: only `libcuda.so.1` (NVIDIA driver).

## Key Findings from Prototype

### av1_cuvid duplicate-frame bug — root cause and fix

**Root cause:** `av1_cuvid` decoder wrapper has NVDEC surface management bug → periodic duplicates (~9/300 frames). Setting `hw_device_ctx` + `hw_frames_ctx` is NOT sufficient.

**Fix:** Use native decoder + hwaccel framework:
```c
codec = avcodec_find_decoder_by_name("av1");  // NOT libdav1d, NOT av1_cuvid
avcodec_parameters_to_context(codec_ctx, codecpar);  // extradata required!
codec_ctx->get_format = get_hw_format;          // → AV_PIX_FMT_CUDA
codec_ctx->hw_device_ctx = av_buffer_ref(hw);
avcodec_open2(codec_ctx, codec, nullptr);
// hwaccel (av1_nvdec) activates after first frame — codec_ctx->hwaccel
// is NULL until then.
```

Verified: 7202 frames, 0 duplicates (`tests/test_decoder.cpp`).

### Codec selection rules

| Codec | HW path | SW fallback | Never use |
|-------|---------|-------------|-----------|
| AV1 | `av1` + `av1_nvdec` hwaccel | `libdav1d` | `av1_cuvid` |
| H.264 | `h264` + `h264_nvdec` hwaccel | software | `h264_cuvid` |
| HEVC | `hevc` + `hevc_nvdec` hwaccel | software | `hevc_cuvid` |

**Golden rule:** Native decoders + get_format hwaccel. Never `_cuvid` variants.

### Other findings

- VFX SDK bundles all deps (TensorRT, NPP, cuDNN). Only `libcuda.so.1` is external.
- VSR internal CUDA streams require device-level sync, not stream-level.
- `hwaccel` field on AVCodecContext is NULL after `avcodec_open2` — set after first frame.
- `avcodec_parameters_to_context()` is REQUIRED for hwaccel init (provides extradata).

## References

- [NVIDIA Video Effects SDK](https://developer.nvidia.com/video-effects-sdk)
- [NVIDIA Maxine Linux VFX SDK (EA) on NGC](https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/collections/maxine_linux_vfx_sdk_collection_ea)
- [FFmpeg hw_decode.c — CUDA decode example](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c)
