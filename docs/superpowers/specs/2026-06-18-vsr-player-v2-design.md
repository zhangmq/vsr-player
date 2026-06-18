# VSR Player v2 Design

**Date:** 2026-06-18
**Status:** approved

## Goal

Upgrade the VSR player prototype to a proper video player with:
1. Optimized pipeline performance (async CUDA streams, double-buffering)
2. Adaptive VSR scaling based on window size
3. GPU-native rendering via OpenGL (remove CPU display bottleneck)

## Architecture

```
cv2.VideoCapture → frame_to_gpu (async H2D) → VSR (non_blocking, CUDA stream)
→ DLPack clone → CUDA-GL interop → GL texture → GL fullscreen render + overlay
```

Double-buffer pipeline: VSR processes frame N on CUDA stream A while CPU decodes frame N+1 and stages the upload. CUDA-OpenGL interop eliminates the D2H path entirely — the VSR output tensor maps directly to a GL texture.

## Module Layout

```
src/vsr_player/
├── __main__.py       # Entry point + argparse (--quality only, no --scale)
├── decoder.py         # cv2.VideoCapture wrapper, prefetch next frame
├── vsr_pipeline.py    # frame_to_gpu, vsr.run(async), clone, texture prep
├── renderer.py        # GLFW + PyOpenGL, fullscreen window, texture blit
├── overlay.py         # GL-drawn UI: play/pause, exit button, status text
├── app.py             # Main loop: coordinate decoder → VSR → renderer
└── config.py          # Adaptive scale calculator, quality constants
```

## Adaptive Scaling

- No `--scale` CLI argument. Scale is auto-computed from window dimensions.
- `scale = min(⌊window_w / src_w⌋, ⌊window_h / src_h⌋)`, clamped to [1, 4]
- Debounce: 500ms after last resize event before reloading VSR
- On scale change: set new `output_width`/`output_height`, call `vsr.load()`
- Initial: fullscreen window → one-time scale calculation

## Async Pipeline

- H2D: `torch.from_numpy(frame).cuda(non_blocking=True)`, frame originates from pinned memory for faster transfer
- VSR: `vsr.run(gpu_in, non_blocking=True, stream_ptr=stream.cuda_stream)`
- Sync point: `stream.synchronize()` after clone
- CUDA-GL interop: register VSR output buffer with `torch.cuda.Graph` or manual `cudaGraphicsGLRegisterBuffer`, map to GL PBO, bind as texture

## GL Renderer

- GLFW window, fullscreen by default, resizable
- Fullscreen quad with the VSR output texture
- Overlay drawn in the same GL context: bottom bar with alpha-blended background, play/pause toggle button, exit button, status text (scale, quality, framerate)
- Mouse click callback for button hit testing
- Window resize callback for adaptive scale trigger
- Key bindings: SPACE = play/pause, Q/ESC = quit, F = toggle fullscreen

## Dependencies

| Package | Purpose |
|---------|---------|
| `nvidia-vfx` | VideoSuperRes effect |
| `torch` | GPU tensor, CUDA streams, CUDA-GL interop |
| `opencv-python` | Video decode (CPU) |
| `glfw` | Window management |
| `PyOpenGL` | OpenGL rendering |

## Out of Scope

- Audio playback (next iteration)
- Interactive seek/scrub controls
- Hardware-accelerated video decode
- Multiple quality presets at runtime
- Configuration file persistence
- Drag-and-drop file open
