# Architecture

## Overview

VSR Player is a single-process, two-layer application. The Qt client (main thread, QML UI) communicates with the playback engine (worker threads, libvsrplayer) through a thread-safe command/event queue. All video frames stay on GPU — from NVDEC decode through AI upscale to Vulkan presentation.

## Full Architecture Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│  Qt Client (main thread, Qt event loop)                          │
│                                                                  │
│  QQuickView                                                     │
│  ├── TopBar                                                      │
│  ├── BottomBar (transport, volume, quality, speed, playlist)     │
│  ├── VolumePopup / QualityPopup / SpeedPopup                     │
│  ├── PlaylistPanel (Drawer + ListView)                           │
│  ├── ProgressSlider                                              │
│  ├── CenterPlayBtn                                               │
│  ├── OsdOverlay (Tab toggle)                                     │
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
└──────────────────────────────────────────────────────────────────┘
```

## Data Flow

### Video Path (all GPU, zero PCIe copy)

```
Container ──[FFmpeg]──► NVDEC ──► NV12(GPU)
  → NV12→RGB(GPU) → VSR → RGBA(GPU)
  → CUDA-Vulkan interop → Vulkan render pass → screen
```

1. **Demux** — libavformat reads container, extracts video packets
2. **Decode** — NVDEC (via FFmpeg hwaccel framework) decodes to NV12 surfaces on GPU
3. **NV12→RGB** — Custom CUDA kernel converts NV12 to RGB float32
4. **VSR** — NvVFX SDK upscales/denoises the RGB frame via Tensor Cores
5. **Render** — CUDA-Vulkan external memory interop makes the RGBA frame available to Vulkan without copying; Vulkan render pass with custom fragment shader presents to screen

### Audio Path

```
Container ──[FFmpeg]──► PCM ──► PortAudio ──► speakers
                                    │
                                    └──► master clock → A/V sync
```

- Audio decoded to PCM by FFmpeg
- PortAudio plays PCM in a callback-driven stream
- Audio clock (sample position / sample rate) provides the master timebase
- Video frames are presented or dropped based on PTS comparison with audio clock

## Module Responsibilities

### PlayerCore
Command queue + playback state machine. Receives `PlayerCommand` variants from client, coalesces related commands, applies state transitions. Runs on a dedicated worker thread. Emits `PlayerEvent` structs to the client via callback.

### Demuxer
FFmpeg libavformat wrapper. Opens containers, discovers streams, reads AVPackets. Supports all formats FFmpeg handles.

### Decoder
FFmpeg libavcodec wrapper with NVDEC hardware acceleration. Uses the **native decoder + hwaccel framework** (not `_cuvid` variants). Maintains dual-context architecture: can switch between hardware (NVDEC) and software decode at runtime without reloading the file. Provides CUDA surfaces directly.

### VSRProcessor
NvVFX VideoSuperRes C API wrapper. Configures upscale factor (2×/3×/4×) and quality level. Supports an optional denoise-only pass (scale=1). Loads the full VFX dependency chain (TensorRT, cuDNN, NPP). Internal CUDA streams require device-level synchronization.

### Renderer
Vulkan render pipeline. Manages: descriptor sets, graphics pipelines (VSR path and NO-VSR path), CUDA-Vulkan interop textures via `VK_KHR_external_memory_fd`. Records Vulkan command buffers each frame with Qt's `beforeRenderPassRecording` signal.

### AudioOutput
PortAudio wrapper. Opens PCM stream with format/sample rate/channel count from the source. Supports dynamic playback speed control. Provides the audio clock for A/V sync.

### ClockManager
Audio-master A/V synchronization (mpv/VLC model). Video frames are presented when their PTS matches the audio clock position. Frames too far behind the clock are dropped. Frames ahead of the clock wait (capped to prevent stalls).

### NV12ToRGB
Standalone CUDA kernel compiled at runtime via NVRTC. Converts NV12 (Y + interleaved UV) to RGB float32 [0,1] on GPU. Handles row pitch/stride differences between NVDEC and CUDA.

### FramePool
GPU frame buffer manager. Pre-allocates CUDA memory for intermediate buffers (NV12 staging, RGB float32, RGBA uint8). Reuses buffers across frames to eliminate per-frame allocation.

## Key Design Decisions

### Codec Selection: Native + hwaccel, never `_cuvid`

| Codec | HW path | SW fallback | Never use |
|-------|---------|-------------|-----------|
| AV1 | `av1` + `av1_nvdec` hwaccel | `libdav1d` | `av1_cuvid` |
| H.264 | `h264` + `h264_nvdec` hwaccel | software | `h264_cuvid` |
| HEVC | `hevc` + `hevc_nvdec` hwaccel | software | `hevc_cuvid` |

The `_cuvid` variants have a surface management bug causing periodic duplicate frames (~9/300). Native decoder + `get_format` hwaccel is correct and verified (7202 frames, 0 duplicates).

### Vulkan + Wayland Judgment Chain

NVIDIA proprietary driver supports `VK_KHR_wayland_surface` and native Wayland Vulkan presentation **if** `nvidia_drm.modeset=1` (kernel cmdline). Check with:

```bash
sudo cat /sys/module/nvidia_drm/parameters/modeset  # must be Y
```

Without modesetting, `vkGetPhysicalDeviceSurfaceSupportKHR` returns `VK_FALSE` for all queues. Fallback: `QT_QPA_PLATFORM=xcb` (XWayland).

### VSR Dependency Chain

```
libnvVFXVideoSuperRes.so (62K)     ← VSR C API, our direct dependency
  ├── libVideoFX.so (40K)           ← VFX core framework
  ├── libVideoFXLocal.so (5.8M)     ← local processing pipeline
  ├── libNVCVImage.so (4.4M)        ← CUDA image utilities
  ├── libnvngxruntime.so (79K)      ← NGX runtime loader
  │     └── libnvidia-ngx-vsr.so (44M)  ← NGX VSR inference engine
  │           ├── libnvinfer.so.10 (641M)       ← TensorRT
  │           ├── libnvinfer_plugin.so.10 (53M) ← TensorRT plugins
  │           ├── libnvonnxparser.so.10 (4.3M)  ← ONNX model parser
  │           ├── libcudnn.so.9 (123K)          ← cuDNN
  │           └── libnpp*.so.12 ×9 (292M)       ← CUDA NPP
  └── libcuda.so.1                   ← NVIDIA driver (system, only external dep)
```

Total bundled: ~1.1 GB. Shipped in `build/lib/`, RPATH `$ORIGIN/lib`. No CUDA Toolkit, TensorRT SDK, or cuDNN system installation needed.

### SDK Isolation

All VFX SDK dependencies are bundled with the application. The only system-level requirement is the NVIDIA driver (`libcuda.so.1`). The build system links against `libcuda.so.1` from the system and the VFX libraries from `third_party/nvvfx/lib/`.

## File Index

### src/client/ (Qt Client)

| File | Purpose |
|------|---------|
| `main.cpp` | Entry point, QQuickView + Vulkan init, event loop |
| `PlayerViewModel.h/cpp` | QML ↔ Core property/signal bridge |
| `PlaylistEngine.h/cpp` | Folder scanner, file list model for QML |
| `KeyFilter.h/cpp` | Keyboard event filter → ViewModel actions |
| `QtVulkanContext.h/cpp` | RAII Vulkan instance + device wrapper |
| `shaders/` | GLSL vertex/fragment shaders |
| `ui/overlay.qml` | Main QML overlay — component wiring layer |
| `ui/components/IconButton.qml` | Reusable icon/label button component |

### src/core/ (Playback Engine)

| File | Purpose |
|------|---------|
| `api/Player.h` | Public API — `Player` interface, `PlayerCommand`, `PlayerEvent` |
| `PlayerCore.h/cpp` | Implementation — command queue, state machine, pipeline orchestration |
| `Demuxer.h/cpp` | FFmpeg avformat container demuxer |
| `Decoder.h/cpp` | NVDEC hwaccel decoder with dual-context (HW/SW switch) |
| `VSRProcessor.h/cpp` | NvVFX VideoSuperRes wrapper — upscale + denoise |
| `Renderer.h/cpp` | Vulkan render pipeline — descriptors, pipelines, interop |
| `AudioOutput.h/cpp` | PortAudio PCM output with speed control |
| `ClockManager.h/cpp` | Audio-master A/V sync (PTS-based) |
| `FramePool.h/cpp` | GPU frame buffer manager (pre-allocated, reused) |
| `utils/CUDAContext.h/cpp` | CUDA device context RAII |
| `utils/VulkanContext.h/cpp` | Vulkan instance/device RAII |
| `utils/NV12ToRGB.h/cpp` | CUDA kernel NV12→RGB (NVRTC runtime compile) |
