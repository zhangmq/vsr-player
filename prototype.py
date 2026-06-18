"""VSR Player prototype — real-time video super-resolution playback."""
import sys
import argparse
import cv2
import torch
from nvvfx import VideoSuperRes

# ── global UI state (accessed by mouse callback) ──
_playing = True
_button_rects = {}  # label -> (x, y, w, h)


def frame_to_gpu(bgr_uint8):
    """Convert OpenCV BGR uint8 (H,W,3) -> GPU RGB float32 (3,H,W) in [0,1]."""
    tensor = torch.from_numpy(bgr_uint8).cuda()
    tensor = tensor[..., [2, 1, 0]]          # BGR -> RGB
    tensor = tensor.float() / 255.0
    tensor = tensor.permute(2, 0, 1)         # HWC -> CHW
    return tensor.contiguous()


def gpu_to_display(rgb_float32):
    """Convert GPU RGB float32 (3,H,W) in [0,1] -> OpenCV BGR uint8 (H,W,3)."""
    tensor = rgb_float32.permute(1, 2, 0)    # CHW -> HWC
    tensor = tensor * 255.0
    tensor = tensor.clamp(0, 255).to(torch.uint8)
    tensor = tensor[..., [2, 1, 0]]          # RGB -> BGR
    return tensor.cpu().numpy()


def mouse_callback(event, x, y, flags, param):
    global _playing
    if event != cv2.EVENT_LBUTTONDOWN:
        return

    for label, (bx, by, bw, bh) in _button_rects.items():
        if bx <= x <= bx + bw and by <= y <= by + bh:
            if label == "playpause":
                _playing = not _playing
            elif label == "exit":
                _playing = False  # stop playback
                cv2.destroyAllWindows()
            return


def draw_overlay(frame, fps, quality_name, scale):
    """Draw semi-transparent control bar + buttons at the bottom of the frame."""
    global _button_rects
    h, w = frame.shape[:2]
    bar_h = 80
    bar_y = h - bar_h
    alpha = 0.5  # bar transparency

    # ── semi-transparent bar via alpha blending ──
    bar_bg = frame[bar_y:h, :].astype(float)
    bar_overlay = bar_bg * alpha + 20 * (1 - alpha)  # dark grey tone
    frame[bar_y:h, :] = bar_overlay.astype(frame.dtype)

    # ── play/pause button ──
    btn_w, btn_h = 60, 40
    btn_x, btn_y = 20, bar_y + (bar_h - btn_h) // 2
    symbol = "||" if _playing else ">"
    cv2.rectangle(frame, (btn_x, btn_y), (btn_x + btn_w, btn_y + btn_h),
                  (70, 130, 180), -1, cv2.LINE_AA)
    cv2.rectangle(frame, (btn_x, btn_y), (btn_x + btn_w, btn_y + btn_h),
                  (100, 160, 210), 2, cv2.LINE_AA)
    (tw, th), _ = cv2.getTextSize(symbol, cv2.FONT_HERSHEY_SIMPLEX, 1.4, 3)
    cv2.putText(frame, symbol,
                (btn_x + (btn_w - tw) // 2, btn_y + (btn_h + th) // 2),
                cv2.FONT_HERSHEY_SIMPLEX, 1.4, (255, 255, 255), 3, cv2.LINE_AA)
    _button_rects["playpause"] = (btn_x, btn_y, btn_w, btn_h)

    # ── status text ──
    status = f"x{scale}  {quality_name}"
    cv2.putText(frame, status, (btn_x + btn_w + 24, bar_y + bar_h // 2 + 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (220, 220, 220), 2, cv2.LINE_AA)

    # ── exit button ──
    exit_x = w - btn_w - 20
    cv2.rectangle(frame, (exit_x, btn_y), (exit_x + btn_w, btn_y + btn_h),
                  (50, 50, 50), -1, cv2.LINE_AA)
    cv2.rectangle(frame, (exit_x, btn_y), (exit_x + btn_w, btn_y + btn_h),
                  (100, 100, 100), 2, cv2.LINE_AA)
    (tw, th), _ = cv2.getTextSize("X", cv2.FONT_HERSHEY_SIMPLEX, 1.0, 3)
    cv2.putText(frame, "X",
                (exit_x + (btn_w - tw) // 2, btn_y + (btn_h + th) // 2),
                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (200, 200, 200), 3, cv2.LINE_AA)
    _button_rects["exit"] = (exit_x, btn_y, btn_w, btn_h)

    # ── fps counter ──
    cv2.putText(frame, f"{fps:.1f} fps | {w}x{h}",
                (exit_x - 220, bar_y + bar_h // 2 + 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (180, 180, 180), 2, cv2.LINE_AA)

    return frame


def main():
    global _playing

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
    print(f"VSR: {in_w}x{in_h} → {out_w}x{out_h} (×{scale}, {quality.name})")
    print("Click Play/Pause to toggle, X to exit.")
    print("Keys: SPACE=pause, Q/ESC=quit")

    cv2.namedWindow("VSR Player", cv2.WINDOW_NORMAL)
    cv2.setWindowProperty("VSR Player", cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    cv2.setMouseCallback("VSR Player", mouse_callback)

    _playing = True

    try:
        while True:
            # ── pause loop ──
            while not _playing:
                try:
                    key = cv2.waitKey(100) & 0xFF
                    if key in (27, ord('q')):
                        raise StopIteration
                    if key == ord(' '):
                        _playing = not _playing
                    # Window might have been closed via overlay exit button
                    if cv2.getWindowProperty("VSR Player", cv2.WND_PROP_VISIBLE) < 1:
                        raise StopIteration
                except Exception:
                    break

            ret, frame = cap.read()
            if not ret:
                break

            gpu_in = frame_to_gpu(frame)
            result = vsr.run(gpu_in)
            gpu_out = torch.from_dlpack(result.image).clone()
            display_frame = gpu_to_display(gpu_out)

            # Draw overlay controls
            display_frame = draw_overlay(display_frame, fps, quality.name, scale)

            cv2.imshow("VSR Player", display_frame)

            key = cv2.waitKey(frame_delay_ms) & 0xFF
            if key in (27, ord('q')):
                break
            if key == ord(' '):
                _playing = not _playing
    except StopIteration:
        pass
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
