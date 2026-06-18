"""VSR Player prototype — real-time video super-resolution playback."""
import sys
import argparse
import cv2
import torch
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
    frame_delay_ms = round(1000 / fps)

    scale = args.scale
    quality = quality_map[args.quality]
    out_w = in_w * scale
    out_h = in_h * scale

    vsr = VideoSuperRes(quality=quality)
    vsr.output_width = out_w
    vsr.output_height = out_h
    vsr.load()

    print(f"Playing: {args.video_file} ({in_w}x{in_h}, {fps:.1f} fps)")
    print(f"VSR: {in_w}x{in_h} → {out_w}x{out_h} (x{scale}, {quality.name})")
    print("Press Q or ESC to quit.")

    try:
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
    finally:
        cap.release()
        cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
