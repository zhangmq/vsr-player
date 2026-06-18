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
        """Seek to approximate frame position using time-based seek."""
        if pos_frames <= 0:
            # Seek to beginning: re-open (PyAV container seek to 0 is unreliable)
            self._container.close()
            self._open("cuda" if self._using_hw else None)
            return

        target_sec = pos_frames / self.fps
        # Convert to stream time_base units
        ts = self._video_stream.time_base
        if ts is not None:
            target_ts = int(target_sec / ts)
        else:
            target_ts = int(target_sec * 1_000_000)
        try:
            self._container.seek(target_ts, stream=self._video_stream)
        except Exception:
            pass
        # Reset decode iterator after seek
        self._decode_iter = self._container.decode(self._video_stream)
        self._next_frame = None

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
