# VSR Player Prototype Design

**Date:** 2026-06-18
**Status:** approved

## Goal

Verify that the `nvidia-vfx` VideoSuperRes (VSR) effect works in a real-time playback pipeline. Produce a minimal working prototype — a single-file Python script that plays a local video file through GPU-accelerated super-resolution, displayed in a window. No audio, no interactive controls, no hardware-accelerated decode.

## Architecture

Single file `prototype.py`, ~80-100 lines, using OpenCV + PyTorch + nvidia-vfx.

```
CLI args (argparse)
  ├── video_file (positional), --scale (2), --quality (HIGH)
  │
VideoSuperRes init
  ├── Read first frame to get input resolution
  ├── Set output_width/output_height → load()
  │
Main loop (per-frame)
  ├── cv2.read() → numpy BGR uint8 (HWC, CPU)
  ├── GPU transform: BGR→RGB, uint8→f32[0,1], HWC→CHW (torch)
  ├── vsr.run(tensor) → DLPack → torch.from_dlpack().clone()
  ├── Inverse: CHW→HWC, f32→uint8, RGB→BGR, GPU→CPU
  ├── cv2.imshow() + waitKey(frame_interval)
  └── Exit on ESC/Q or window close
```

Key point: all format conversion happens on GPU. Only two CPU↔GPU copies per frame (numpy→torch at start, clone→numpy at end). VSR input/output stay on CUDA.

## CLI Interface

```
python prototype.py <video_file> [--scale 2] [--quality HIGH]
```

| Argument | Type | Default | Description |
|----------|------|---------|-------------|
| `video_file` | positional | required | Path to local video |
| `--scale` | int | 2 | Scale factor: 1, 2, 3, or 4 |
| `--quality` | str | HIGH | VSR quality: LOW, MEDIUM, HIGH, ULTRA |

When `--scale 1`, output dimensions equal input dimensions — no upscaling, VSR still processes for artifact cleanup (equivalent to a mild enhancement pass).

## Error Handling

- Missing/unreadable file → print error, exit(1)
- No NVIDIA GPU / driver too old → nvvfx raises on load(), print driver requirements, exit(1)
- output_width/output_height = 0 → guard before load(), print message, exit(1)
- Playback speed: `cv2.waitKey` delay matches source video FPS (`1000/fps ms`)
- Exit: ESC key, 'q' key, closing window (X button), or SIGINT (Ctrl+C) — all clean exit

No graceful degradation or recovery. Prototype crashes on unexpected conditions.

## Constraints

- **GPU:** NVIDIA with Tensor Cores (Turing/Ampere/Ada/Blackwell/Hopper)
- **Driver:** Linux 570.190+ (verified: RTX 5060 Ti, 610.43.02)
- **Python:** 3.12 in mamba env `vsr-player`
- **OS:** Linux (CachyOS, X11/Wayland via OpenCV highgui)

## Dependencies

| Package | Purpose |
|---------|---------|
| `nvidia-vfx` | VideoSuperRes effect |
| `torch` | GPU tensor management, DLPack interop |
| `opencv-python` | Video decode, display window |

## Out of Scope

- Audio playback
- Interactive controls (pause, seek, volume)
- Hardware-accelerated video decode (NVDEC)
- GUI/controls overlay
- Drag-and-drop file open
- Camera/stream input
- Configuration file / settings persistence
- Multiple quality presets at runtime
