# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Linux desktop video player that applies real-time AI super-resolution to video playback using the [nvidia-vfx](https://pypi.org/project/nvidia-vfx/) library (NVIDIA Video Effects SDK Python bindings). The player decodes video, upscales/denoises each frame on the GPU via Tensor Cores, and displays the enhanced output — all in real time during playback.

## Build / Run / Test

```bash
# Activate environment
mamba activate vsr-player

# Install dependencies (once)
pip install torch opencv-python nvidia-vfx

# Run prototype player
python prototype.py <video_file> [--scale 2] [--quality HIGH]

# Lint
ruff check .

# Run tests (when test suite exists)
pytest
```

## Architecture

The core data flow is a real-time pipeline:

```
Video File → Demux → Decode (HW) → GPU Tensor → VSR Effect → GPU Tensor → Display
                                    ↑ (3,H,W) float32 [0,1]      ↓ DLPack → clone
                              Audio Track → Audio Sink (separate path)
```

### Key Pipeline Stages

1. **Demux & Decode** — Extract video frames from the container (mp4/mkv/webm). Use hardware-accelerated decode (NVDEC via FFmpeg) to keep frames on GPU and avoid PCIe round-trips.

2. **Frame Conversion** — Decoded frames arrive as NV12/YUV on GPU. Convert to channels-first RGB float32 `(3, H, W)` normalized to `[0,1]` to match nvidia-vfx input requirements. Keep the tensor on CUDA throughout.

3. **VSR Processing** — Apply `nvvfx.VideoSuperRes` effect. Set `output_width`/`output_height` before `load()`. Call `run(frame)` per frame. **Critical:** clone the output immediately (`torch.from_dlpack(result.image).clone()`) — the internal buffer is reused on the next `run()` call.

4. **Display** — Convert back to a format the display layer consumes (RGBA uint8 for OpenGL/Vulkan, or BGR for simpler backends). Render with presentation timestamps from the source file to maintain A/V sync.

5. **Audio** — Demuxed audio passes through to the audio backend (PulseAudio/ALSA/PipeWire) independently. Audio clock serves as the master clock for A/V sync.

### Module Layout (planned)

```
src/vsr_player/
├── __main__.py        # Entry point, CLI arg parsing
├── app.py             # Application/playback controller, A/V sync loop
├── pipeline.py        # Per-frame VSR pipeline: convert → VSR → convert
├── decoder.py         # Video demux + HW decode (NVDEC via PyAV or FFmpeg)
├── display.py         # Frame presentation (OpenGL render or fallback)
├── audio.py           # Audio output (PulseAudio/pipewire-pulse)
├── clock.py           # Master clock tied to audio, used for A/V sync
└── config.py          # QualityLevel mapping, scale factors, defaults
```

### Critical GPU Buffer Rules (nvidia-vfx)

- Input: float32 RGB (3, H, W) on CUDA, range [0.0, 1.0]
- Output: DLPack capsule → **must clone** before next `run()` — use `.clone()` (PyTorch) or `.copy()` (CuPy)
- For real-time, use `non_blocking=True` + CUDA streams to overlap VSR with decode
- Call `load()` once after setting dimensions/quality; reuse the `VideoSuperRes` instance across all frames — never reload per frame

### Quality Levels (nvidia-vfx)

| Group | Levels (IntEnum 0–19) | Purpose |
|---|---|---|
| Standard Upscaling | BICUBIC, LOW, MEDIUM, HIGH, ULTRA | Upscale compressed video |
| Denoise | DENOISE_LOW … DENOISE_ULTRA | Same-resolution noise reduction |
| Deblur | DEBLUR_LOW … DEBLUR_ULTRA | Same-resolution sharpening |
| High-bitrate | HIGHBITRATE_LOW … HIGHBITRATE_ULTRA | Upscale clean/lossless sources |

## Constraints & Requirements

- **GPU:** NVIDIA with Tensor Cores (Turing/Ampere/Ada/Blackwell/Hopper)
- **Driver (Linux):** 570.190+ or 580.82+ or 590.44+
- **Python:** 3.10+
- **OS:** Ubuntu 20.04/22.04/24.04, Debian 12, RHEL 8/9
- VSR processing is the bottleneck; expect ~2–10ms per frame depending on quality level and resolution
- Keep PCIe transfers minimal — decode → VSR → display should ideally stay entirely on GPU

## References

- [nvidia-vfx on PyPI](https://pypi.org/project/nvidia-vfx/)
- [Official API Reference](https://docs.nvidia.com/maxine/vfx-python/latest/index.html)
- [NVIDIA VFX Python Samples](https://github.com/NVIDIA-Maxine/nvidia-vfx-python-samples)
