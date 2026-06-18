"""VSR Player prototype — real-time video super-resolution playback."""
import sys
import cv2
import torch
import numpy as np
from nvvfx import VideoSuperRes


def frame_to_gpu(bgr_uint8):
    """Convert OpenCV BGR uint8 (H,W,3) -> GPU RGB float32 (3,H,W) in [0,1]."""
    tensor = torch.from_numpy(bgr_uint8).cuda()  # (H, W, 3) uint8
    tensor = tensor[..., [2, 1, 0]]              # BGR -> RGB
    tensor = tensor.float() / 255.0              # uint8 -> f32 [0,1]
    tensor = tensor.permute(2, 0, 1)             # HWC -> CHW
    return tensor


def gpu_to_display(rgb_float32):
    """Convert GPU RGB float32 (3,H,W) in [0,1] -> OpenCV BGR uint8 (H,W,3)."""
    tensor = rgb_float32.permute(1, 2, 0)        # CHW -> HWC
    tensor = tensor * 255.0                       # f32 [0,1] -> [0,255]
    tensor = tensor.clamp(0, 255).to(torch.uint8)
    tensor = tensor[..., [2, 1, 0]]              # RGB -> BGR
    return tensor.cpu().numpy()


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

    print(f"VSR: {in_w}x{in_h} -> {out_w}x{out_h} (x{scale}, {quality.name})")

    print(f"Playing: {path} ({int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x"
          f"{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}, {fps:.1f} fps)")
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
        if key in (27, ord('q')):  # ESC or Q
            break

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
