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
        self._cu_codec_ctx = None

        self._open(hw_device)

    # ── Open / init ──────────────────────────────────────────────────

    def _open(self, hw_device: Optional[str]):
        """Open container. Try HWAccel → explicit _cuvid → software."""
        # 1. Try HWAccel (works for H264, HEVC)
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

        # 2. Try explicit _cuvid codec (AV1, VP9, etc.)
        if self._container is None and hw_device is not None:
            try:
                self._container = av.open(self._path)
                sw_vs = self._container.streams.video[0]
                cuvid = _find_cuvid(sw_vs.codec_context.name)
                if cuvid and cuvid in av.codecs_available:
                    self._cu_codec_ctx = av.Codec(cuvid, 'r').create()
                    self._cu_codec_ctx.width = sw_vs.codec_context.width
                    self._cu_codec_ctx.height = sw_vs.codec_context.height
                    self._video_stream = sw_vs
                    self._using_hw = True
                else:
                    self._container.close()
                    self._container = None
            except Exception:
                if self._container is not None:
                    self._container.close()
                    self._container = None
                self._cu_codec_ctx = None

        # 3. Software fallback
        if self._container is None:
            self._container = av.open(self._path)
            self._video_stream = self._container.streams.video[0]
            self._using_hw = False
            self._cu_codec_ctx = None

        # 4. Populate metadata
        if self._cu_codec_ctx is not None:
            self.width = self._cu_codec_ctx.width
            self.height = self._cu_codec_ctx.height
        else:
            ctx = self._video_stream.codec_context
            self.width = ctx.width
            self.height = ctx.height
        rate = self._video_stream.average_rate
        self.fps = float(rate) if rate is not None and rate > 0 else 30.0
        self.frame_count = self._video_stream.frames or 0

        # 5. Create frame iterator
        if self._cu_codec_ctx is not None:
            self._packet_iter = self._container.demux(self._video_stream)
            self._pending_frames = []
            self._decode_iter = None
        else:
            self._packet_iter = None
            self._pending_frames = None
            self._decode_iter = self._container.decode(self._video_stream)
        self._next_frame = None

    # ── Frame iteration ──────────────────────────────────────────────

    def _next_frame_raw(self):
        """Return the next VideoFrame or raise StopIteration."""
        if self._cu_codec_ctx is not None:
            if self._pending_frames:
                return self._pending_frames.pop(0)
            while True:
                pkt = next(self._packet_iter)
                try:
                    frames = self._cu_codec_ctx.decode(pkt)
                    if frames:
                        self._pending_frames = list(frames)
                        return self._pending_frames.pop(0)
                except Exception:
                    continue
        else:
            return next(self._decode_iter)

    # ── Public API ───────────────────────────────────────────────────

    def read(self) -> Tuple[bool, object]:
        """Read next frame. Returns ``(True, frame)`` or ``(False, None)``."""
        try:
            return True, self._next_frame_raw()
        except StopIteration:
            return False, None
        except Exception:
            return False, None

    def prefetch(self):
        """Prefetch next frame for pipeline overlap."""
        if self._next_frame is None:
            try:
                self._next_frame = (True, self._next_frame_raw())
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
        """Seek to *target_sec* in seconds."""
        self._next_frame = None
        if target_sec <= 0.0:
            self._container.seek(0, stream=self._video_stream)
        else:
            ts = int(target_sec / self.time_base) if self.time_base > 0 else int(target_sec * 1_000_000)
            self._container.seek(ts, stream=self._video_stream)
        self._reset_decode_iter()

        if target_sec <= 0.0:
            return

        # Fast-decode forward to target PTS
        while True:
            try:
                frame = self._next_frame_raw()
                pts = float(frame.pts * self.time_base) if frame.pts is not None else 0
                if pts >= target_sec:
                    self._next_frame = (True, frame)
                    return
            except StopIteration:
                return

    def _reset_decode_iter(self):
        """Recreate the decode iterator after a seek."""
        if self._cu_codec_ctx is not None:
            self._packet_iter = self._container.demux(self._video_stream)
            self._pending_frames = []
            self._decode_iter = None
        else:
            self._decode_iter = self._container.decode(self._video_stream)
            self._packet_iter = None
            self._pending_frames = None

    def release(self):
        if self._container is not None:
            self._container.close()
            self._container = None

    def __del__(self):
        self.release()

    # ── Properties ───────────────────────────────────────────────────

    @property
    def time_base(self) -> float:
        tb = self._video_stream.time_base
        return float(tb) if tb is not None else 0.0

    @property
    def is_hardware(self) -> bool:
        return self._using_hw


# ── Codec name → _cuvid mapping ─────────────────────────────────────

def _find_cuvid(codec_name: str) -> str | None:
    """Map a software codec name to its _cuvid variant, if any."""
    _CVID = {
        'h264': 'h264_cuvid', 'libx264': 'h264_cuvid',
        'hevc': 'hevc_cuvid', 'libx265': 'hevc_cuvid',
        'av1': 'av1_cuvid', 'libdav1d': 'av1_cuvid', 'libaom-av1': 'av1_cuvid',
        'vp8': 'vp8_cuvid', 'vp9': 'vp9_cuvid',
        'mpeg1video': 'mpeg1_cuvid', 'mpeg2video': 'mpeg2_cuvid',
        'mpeg4': 'mpeg4_cuvid', 'vc1': 'vc1_cuvid',
        'mjpeg': 'mjpeg_cuvid',
    }
    return _CVID.get(codec_name)
