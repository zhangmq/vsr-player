"""Video decode wrapper — reads frames from a video file via OpenCV."""

import cv2
from typing import Optional, Tuple


class Decoder:
    """OpenCV-based video decoder with prefetch support."""

    def __init__(self, path: str):
        self._cap = cv2.VideoCapture(path)
        if not self._cap.isOpened():
            raise FileNotFoundError(f"cannot open video: {path}")
        self.width = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.height = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fps = self._cap.get(cv2.CAP_PROP_FPS)
        self.fps = float(fps) if fps > 0 else 30.0
        self.frame_count = int(self._cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self._next_frame: Optional[Tuple[bool, object]] = None

    def read(self):
        """Read next frame. Returns (True, frame) or (False, None)."""
        ret, frame = self._cap.read()
        return ret, frame if ret else None

    def prefetch(self):
        """Read the next frame ahead of time (for pipeline overlap)."""
        if self._next_frame is None:
            ret, frame = self._cap.read()
            self._next_frame = (ret, frame if ret else None)

    def consume_prefetched(self):
        """Return the prefetched frame and clear the slot."""
        if self._next_frame is None:
            return self.read()
        ret_frame = self._next_frame
        self._next_frame = None
        ret, frame = ret_frame
        return ret, frame

    def seek(self, pos_frames: int):
        """Seek to a frame position."""
        self._cap.set(cv2.CAP_PROP_POS_FRAMES, pos_frames)
        self._next_frame = None

    def release(self):
        self._cap.release()

    def __del__(self):
        self.release()
