"""Entry point: python -m vsr_player <video_file> [--quality QUALITY]."""
import argparse
import sys
from .app import App


def main():
    parser = argparse.ArgumentParser(
        description="VSR Player — real-time AI video super-resolution playback"
    )
    parser.add_argument("video_file", help="Path to input video file")
    parser.add_argument("--quality", default="HIGH",
                        choices=["LOW", "MEDIUM", "HIGH", "ULTRA"],
                        help="VSR quality level (default: HIGH)")
    args = parser.parse_args()

    try:
        app = App(args.video_file, args.quality)
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    except RuntimeError as e:
        print(f"Error: {e}")
        sys.exit(1)

    app.run()


if __name__ == "__main__":
    main()
