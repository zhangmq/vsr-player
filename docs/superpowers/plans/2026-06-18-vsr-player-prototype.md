# VSR Player Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimal real-time VSR video player prototype in a single `prototype.py` script.

**Architecture:** OpenCV decode → PyTorch GPU format conversion → nvidia-vfx VSR effect → PyTorch inverse conversion → OpenCV display. Single file, video-only, no audio.

**Tech Stack:** Python 3.12 (mamba env `vsr-player`), nvidia-vfx 0.1.0.1, PyTorch, OpenCV

---

### Task 1: Install dependencies and verify VSR pipeline

**Files:**
- Create: `verify_vsr.py` (temporary smoke-test script, delete after verification)

- [ ] **Step 1: Install PyTorch and OpenCV in mamba env**

```bash
mamba run -n vsr-player pip install torch opencv-python
```

Expected: Both packages install successfully.

- [ ] **Step 2: Write smoke-test to verify full VSR pipeline**

Create `verify_vsr.py`:

```python
"""Smoke-test the nvidia-vfx pipeline. Run once, delete after success."""
import torch
import numpy as np
from nvvfx import VideoSuperRes

def test_vsr_pipeline():
    # Create fake RGB frame (1080p) on GPU
    h, w = 1080, 1920
    frame = torch.rand(3, h, w, device="cuda", dtype=torch.float32)

    # Init VSR at 2x upscale, HIGH quality
    vsr = VideoSuperRes(quality=VideoSuperRes.QualityLevel.HIGH)
    vsr.output_width = w * 2
    vsr.output_height = h * 2
    vsr.load()
    print(f"VSR loaded: {w}x{h} → {vsr.output_width}x{vsr.output_height}")

    # Run VSR on 10 frames to verify stability
    for i in range(10):
        result = vsr.run(frame)
        out = torch.from_dlpack(result.image).clone()
        assert out.shape == (3, h * 2, w * 2), f"Bad output shape: {out.shape}"
        assert out.device.type == "cuda", f"Output not on GPU: {out.device}"

    print(f"OK: 10 frames processed, output shape {out.shape}, device={out.device}")
    return True

if __name__ == "__main__":
    assert torch.cuda.is_available(), "CUDA not available — check GPU driver"
    print(f"GPU: {torch.cuda.get_device_name(0)}")
    test_vsr_pipeline()
    print("All checks passed.")
```

- [ ] **Step 3: Run smoke-test**

```bash
mamba run -n vsr-player python verify_vsr.py
```

Expected output:
```
GPU: NVIDIA GeForce RTX 5060 Ti
VSR loaded: 1920x1080 → 3840x2160
OK: 10 frames processed, output shape torch.Size([3, 2160, 3840]), device=cuda:0
All checks passed.
```

- [ ] **Step 4: Remove smoke-test script**

```bash
rm verify_vsr.py
```

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "chore: verify nvidia-vfx pipeline works with torch GPU tensors"
```

---

### Task 2: Implement basic video decode + display loop

**Files:**
- Create: `prototype.py`

- [ ] **Step 1: Implement minimal decode-and-display loop**

Write `prototype.py`:

```python
"""VSR Player prototype — real-time video super-resolution playback."""
import sys
import cv2

def main():
    if len(sys.argv) < 2:
        print("Usage: python prototype.py <video_file> [--scale N] [--quality Q]")
        sys.exit(1)

    path = sys.argv[1]
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        print(f"Error: cannot open video file: {path}")
        sys.exit(1)

    fps = cap.get(cv2.CAP_PROP_FPS)
    if fps <= 0:
        fps = 30.0
    frame_delay_ms = int(1000 / fps)

    print(f"Playing: {path} ({int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x"
          f"{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}, {fps:.1f} fps)")
    print("Press Q or ESC to quit.")

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        cv2.imshow("VSR Player", frame)

        key = cv2.waitKey(frame_delay_ms) & 0xFF
        if key in (27, ord('q')):  # ESC or Q
            break

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Test with any local video file**

```bash
mamba run -n vsr-player python prototype.py /path/to/any/video.mp4
```

Expected: Window opens, video plays at original resolution, Q/ESC exits cleanly.

- [ ] **Step 3: Commit**

```bash
git add prototype.py && git commit -m "feat: basic video decode + display loop"
```

---

### Task 3: Add VSR processing to the pipeline

**Files:**
- Modify: `prototype.py` — add GPU conversion → VSR → display conversion

- [ ] **Step 1: Add imports and VSR initialization**

Add to top of `prototype.py` after existing imports:

```python
import torch
import numpy as np
from nvvfx import VideoSuperRes
```

- [ ] **Step 2: Add GPU tensor conversion function**

Add after imports, before `main()`:

```python
def frame_to_gpu(bgr_uint8):
    """Convert OpenCV BGR uint8 (H,W,3) → GPU RGB float32 (3,H,W) in [0,1]."""
    tensor = torch.from_numpy(bgr_uint8).cuda()  # (H, W, 3) uint8
    tensor = tensor[..., [2, 1, 0]]              # BGR → RGB
    tensor = tensor.float() / 255.0              # uint8 → f32 [0,1]
    tensor = tensor.permute(2, 0, 1)             # HWC → CHW
    return tensor

def gpu_to_display(rgb_float32):
    """Convert GPU RGB float32 (3,H,W) in [0,1] → OpenCV BGR uint8 (H,W,3)."""
    tensor = rgb_float32.permute(1, 2, 0)        # CHW → HWC
    tensor = tensor * 255.0                       # f32 [0,1] → [0,255]
    tensor = tensor.clamp(0, 255).to(torch.uint8)
    tensor = tensor[..., [2, 1, 0]]              # RGB → BGR
    return tensor.cpu().numpy()
```

- [ ] **Step 3: Add VSR init inside main()**

After opening `cap` and getting resolution, add before the while loop:

```python
    in_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    in_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    scale = 2
    quality = VideoSuperRes.QualityLevel.HIGH

    out_w = in_w * scale
    out_h = in_h * scale

    vsr = VideoSuperRes(quality=quality)
    vsr.output_width = out_w
    vsr.output_height = out_h
    vsr.load()

    print(f"VSR: {in_w}x{in_h} → {out_w}x{out_h} (×{scale}, {quality.name})")
```

- [ ] **Step 4: Insert VSR processing into the frame loop**

Replace the `cv2.imshow("VSR Player", frame)` line:

```python
        gpu_in = frame_to_gpu(frame)
        result = vsr.run(gpu_in)
        gpu_out = torch.from_dlpack(result.image).clone()
        display_frame = gpu_to_display(gpu_out)

        cv2.imshow("VSR Player", display_frame)
```

- [ ] **Step 5: Add cleanup after the loop**

After `cv2.destroyAllWindows()`, before the closing of `main()`:

```python
```

(The VSR instance cleans itself up on GC. No explicit close needed.)

- [ ] **Step 6: Run and visually verify**

```bash
mamba run -n vsr-player python prototype.py /path/to/any/video.mp4
```

Expected: Window opens at 2× resolution (e.g., 720p → 1440p). Video plays smoothly. Q/ESC exits.

- [ ] **Step 7: Commit**

```bash
git add prototype.py && git commit -m "feat: integrate nvidia-vfx VSR into playback pipeline"
```

---

### Task 4: Add CLI argument parsing

**Files:**
- Modify: `prototype.py` — add argparse

- [ ] **Step 1: Add argparse and integrate parameters**

Replace the `main()` function with the full CLI-aware version:

```python
import argparse

def main():
    parser = argparse.ArgumentParser(
        description="VSR Player — real-time video super-resolution playback"
    )
    parser.add_argument("video_file", help="Path to input video file")
    parser.add_argument("--scale", type=int, default=2, choices=[1, 2, 3, 4],
                        help="Upscale factor (default: 2)")
    parser.add_argument("--quality", default="HIGH",
                        choices=["LOW", "MEDIUM", "HIGH", "ULTRA"],
                        help="VSR quality level (default: HIGH)")
    args = parser.parse_args()

    quality_map = {
        "LOW": VideoSuperRes.QualityLevel.LOW,
        "MEDIUM": VideoSuperRes.QualityLevel.MEDIUM,
        "HIGH": VideoSuperRes.QualityLevel.HIGH,
        "ULTRA": VideoSuperRes.QualityLevel.ULTRA,
    }

    cap = cv2.VideoCapture(args.video_file)
    if not cap.isOpened():
        print(f"Error: cannot open video file: {args.video_file}")
        sys.exit(1)

    in_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    in_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    if fps <= 0:
        fps = 30.0
    frame_delay_ms = int(1000 / fps)

    scale = args.scale
    quality = quality_map[args.quality]
    out_w = in_w * scale
    out_h = in_h * scale

    vsr = VideoSuperRes(quality=quality)
    vsr.output_width = out_w
    vsr.output_height = out_h
    vsr.load()

    print(f"Playing: {args.video_file} ({in_w}x{in_h}, {fps:.1f} fps)")
    print(f"VSR: {in_w}x{in_h} → {out_w}x{out_h} (×{scale}, {quality.name})")
    print("Press Q or ESC to quit.")

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        gpu_in = frame_to_gpu(frame)
        result = vsr.run(gpu_in)
        gpu_out = torch.from_dlpack(result.image).clone()
        display_frame = gpu_to_display(gpu_out)

        cv2.imshow("VSR Player", display_frame)

        key = cv2.waitKey(frame_delay_ms) & 0xFF
        if key in (27, ord('q')):
            break

    cap.release()
    cv2.destroyAllWindows()
```

- [ ] **Step 2: Test all CLI flag combinations**

```bash
# Default (2x HIGH)
mamba run -n vsr-player python prototype.py /path/to/video.mp4

# 4x ULTRA
mamba run -n vsr-player python prototype.py /path/to/video.mp4 --scale 4 --quality ULTRA

# 1x (same-resolution, artifact cleanup)
mamba run -n vsr-player python prototype.py /path/to/video.mp4 --scale 1 --quality HIGH

# Invalid args (should fail gracefully)
mamba run -n vsr-player python prototype.py --scale 5 /path/to/video.mp4
mamba run -n vsr-player python prototype.py /path/to/video.mp4 --quality MAXIMUM
mamba run -n vsr-player python prototype.py /nonexistent.mp4
```

Expected: Valid combos play correctly. Invalid combos exit with argparse error message.

- [ ] **Step 3: Commit**

```bash
git add prototype.py && git commit -m "feat: add CLI argument parsing with scale and quality flags"
```

---

### Task 5: Final integration test and CLAUDE.md update

**Files:**
- Modify: `CLAUDE.md` — update run command to match prototype

- [ ] **Step 1: Update CLAUDE.md with prototype run instructions**

In `CLAUDE.md`, replace the "Build / Run / Test" section with:

```markdown
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
```

- [ ] **Step 2: Run final end-to-end smoke test**

```bash
# Verify with a real video file
mamba run -n vsr-player python prototype.py /path/to/some/video.mp4 --scale 2 --quality HIGH
```

Expected: Window appears at 2x resolution, video plays smoothly, Q exits cleanly, no CUDA errors or memory leaks visible in console output.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md && git commit -m "docs: update CLAUDE.md with prototype run instructions"
```
