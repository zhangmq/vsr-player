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
