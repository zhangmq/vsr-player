"""PyAV-based video decoder with NVDEC hardware acceleration and SW fallback."""

import av
import av.codec.hwaccel as hw
from typing import Optional, Tuple


class Decoder:
    """PyAV decoder returning :class:`av.VideoFrame` objects.

    Tries NVDEC hardware decode first; falls back to software decode if
    CUDA is unavailable or the codec is unsupported.

    Attributes:
        width, height (int): video dimensions
        fps (float): frames per second (≥1.0)
        frame_count (int): total frames (0 if unknown)
        is_hardware (bool): True if NVDEC is active
    """

    def __init__(self, path: str, hw_device: Optional[str] = "cuda"):
        self._path = path
        self._container = None
        self._video_stream = None
        self._decode_iter = None
        self._using_hw = False
        self._next_frame: Optional[Tuple[bool, object]] = None

        self._open(hw_device)

    def _open(self, hw_device: Optional[str]):
        """Open container. Try HW decode first, fall back to software."""
        # ── 1. Attempt hardware decode ──
        if hw_device is not None:
            try:
                accel = hw.HWAccel(hw_device)
                self._container = av.open(self._path, hwaccel=accel)
                vs = self._container.streams.video[0]
                if vs.codec_context.is_hwaccel:
                    self._video_stream = vs
                    self._using_hw = True
                else:
                    self._container.close()
                    self._container = None
            except Exception:
                if self._container is not None:
                    self._container.close()
                    self._container = None

        # ── 2. Fall back to software decode ──
        if self._container is None:
            self._container = av.open(self._path)
            vs = self._container.streams.video[0]
            self._video_stream = vs
            self._using_hw = False

        # ── 3. Populate metadata ──
        ctx = self._video_stream.codec_context
        self.width = ctx.width
        self.height = ctx.height
        rate = self._video_stream.average_rate
        self.fps = float(rate) if rate is not None and rate > 0 else 30.0
        self.frame_count = self._video_stream.frames or 0
        self._decode_iter = self._container.decode(self._video_stream)
        self._next_frame = None

    # ── Public API (same as old OpenCV Decoder) ──────────────────────

    def read(self) -> Tuple[bool, object]:
        """Read next frame. Returns ``(True, frame)`` or ``(False, None)``."""
        try:
            frame = next(self._decode_iter)
            return True, frame
        except StopIteration:
            return False, None
        except Exception:
            return False, None

    def prefetch(self):
        """Prefetch next frame for pipeline overlap (retained for API compat)."""
        if self._next_frame is None:
            try:
                frame = next(self._decode_iter)
                self._next_frame = (True, frame)
            except StopIteration:
                self._next_frame = (False, None)
            except Exception:
                self._next_frame = (False, None)

    def consume_prefetched(self) -> Tuple[bool, object]:
        """Return the prefetched frame and clear the cache."""
        if self._next_frame is None:
            return self.read()
        ret_frame = self._next_frame
        self._next_frame = None
        return ret_frame

    def seek(self, pos_frames: int):
        """Seek by frame index (legacy)."""
        if pos_frames <= 0:
            self.seek_seconds(0.0)
            return
        self.seek_seconds(pos_frames / self.fps)

    def seek_seconds(self, target_sec: float):
        """Seek to *target_sec* in seconds.

        Uses ``container.seek()`` to land on the nearest keyframe before
        *target_sec*, then fast-decodes forward to the exact target PTS.
        NVDEC at 3000+ fps makes the decode-forward step negligible.
        """
        self._next_frame = None

        if target_sec <= 0.0:
            self._container.seek(0, stream=self._video_stream)
            self._decode_iter = self._container.decode(self._video_stream)
            return

        # Seek to nearest keyframe before target
        ts = int(target_sec / self.time_base) if self.time_base > 0 else int(target_sec * 1_000_000)
        self._container.seek(ts, stream=self._video_stream)
        self._decode_iter = self._container.decode(self._video_stream)

        # Fast-decode forward to target PTS, stash the target frame
        for frame in self._decode_iter:
            pts = float(frame.pts * self.time_base) if frame.pts is not None else 0
            if pts >= target_sec:
                self._next_frame = (True, frame)
                return
        # Past EOF — iterator exhausted.  Leave _next_frame=None so the
        # main loop hits EOF on the next consume_prefetched() call.

    def release(self):
        if self._container is not None:
            self._container.close()
            self._container = None

    def __del__(self):
        self.release()

    # ── New properties ────────────────────────────────────────────────

    @property
    def time_base(self) -> float:
        """Stream time base in seconds (for PTS conversion)."""
        tb = self._video_stream.time_base
        return float(tb) if tb is not None else 0.0

    @property
    def is_hardware(self) -> bool:
        """True when using NVDEC hardware decode (frames are GPU NV12)."""
        return self._using_hw
