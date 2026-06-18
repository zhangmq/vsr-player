"""Audio playback with sounddevice + ring buffer, providing the master clock.

Decodes audio in a background thread, feeds PCM into a ring buffer consumed
by a sounddevice OutputStream callback.  The audio clock (total samples
played / sample rate) serves as the master clock for A/V sync — matching
the mpv/VLC model.
"""

import threading
import time

import av
import numpy as np
import sounddevice as sd


class _RingBuffer:
    """Single-producer, single-consumer ring buffer for float32 PCM."""

    def __init__(self, capacity_frames: int, channels: int):
        self._buf = np.empty((capacity_frames, channels), dtype=np.float32)
        self._cap = capacity_frames
        self._read = 0       # next frame to consume
        self._write = 0      # next frame to write
        self._filled = 0     # frames available to read
        self._lock = threading.Lock()

    def write(self, data: np.ndarray) -> int:
        """Write interleaved float32 PCM. Returns frames actually written."""
        frames = data.shape[0]
        with self._lock:
            avail = self._cap - self._filled
            n = min(frames, avail)
            if n <= 0:
                return 0
            w = self._write
            if w + n <= self._cap:
                self._buf[w:w + n] = data[:n]
            else:
                first = self._cap - w
                self._buf[w:] = data[:first]
                self._buf[:n - first] = data[first:n]
            self._write = (w + n) % self._cap
            self._filled += n
            return n

    def read(self, dst: np.ndarray):
        """Read up to len(dst) frames into dst. Returns frames actually read."""
        frames = dst.shape[0]
        with self._lock:
            n = min(frames, self._filled)
            if n <= 0:
                return 0
            r = self._read
            if r + n <= self._cap:
                dst[:n] = self._buf[r:r + n]
            else:
                first = self._cap - r
                dst[:first] = self._buf[r:]
                dst[first:n] = self._buf[:n - first]
            self._read = (r + n) % self._cap
            self._filled -= n
            return n

    @property
    def filled(self) -> int:
        return self._filled


class AudioPlayer:
    """Background audio decoder + sounddevice output.

    The audio device drives the master clock.  Call :meth:`start` before
    the main loop and :meth:`stop` on exit.
    """

    def __init__(self, path: str, buffer_sec: float = 0.5):
        self._path = path
        self._sample_rate = 48000
        self._channels = 2
        self._ring: _RingBuffer | None = None
        self._stream: sd.OutputStream | None = None
        self._thread: threading.Thread | None = None
        self._running = False
        self._start_time: float | None = None

        # ── Probe audio stream ──
        container = av.open(path)
        a_stream = None
        for s in container.streams:
            if s.type == "audio":
                a_stream = s
                break
        if a_stream is None:
            container.close()
            return

        ctx = a_stream.codec_context
        self._sample_rate = ctx.sample_rate or 48000
        self._channels = ctx.channels or 2
        # Layout for resampling; sounddevice expects interleaved float32 [-1,1]
        self._resampler = av.AudioResampler(
            format="flt",
            layout="stereo" if self._channels == 2 else f"{self._channels}c",
            rate=self._sample_rate,
        )
        container.close()

        cap = int(self._sample_rate * buffer_sec)
        self._ring = _RingBuffer(cap, self._channels)

    # ── Public API ──────────────────────────────────────────────────

    @property
    def is_active(self) -> bool:
        return self._ring is not None

    @property
    def clock(self) -> float:
        """Master clock in seconds — elapsed time since start."""
        if self._stream is None or self._start_time is None:
            return 0.0
        try:
            t = self._stream.time
            return (t - self._start_time) if t is not None else 0.0
        except Exception:
            return 0.0

    def start(self):
        if self._ring is None:
            return
        self._running = True

        def _callback(outdata, frames, _time_info, _status):
            read = self._ring.read(outdata)
            if read < frames:
                outdata[read:] = 0.0  # silence underrun

        # Pre-buffer: decode a few frames so the callback doesn't start dry
        self._thread = threading.Thread(target=self._decode_loop, daemon=True)
        self._thread.start()
        while self._ring.filled < self._sample_rate // 4:  # ~250ms
            time.sleep(0.005)

        self._stream = sd.OutputStream(
            samplerate=self._sample_rate,
            channels=self._channels,
            dtype="float32",
            blocksize=1024,
            callback=_callback,
        )
        self._stream.start()
        self._start_time = self._stream.time

    def stop(self):
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None

    # ── Decode thread ───────────────────────────────────────────────

    def _decode_loop(self):
        container = av.open(self._path)
        try:
            a_stream = None
            for s in container.streams:
                if s.type == "audio":
                    a_stream = s
                    break
            if a_stream is None:
                return

            for frame in container.decode(a_stream):
                if not self._running:
                    break
                # Resample / convert to interleaved float32
                resampled = self._resampler.resample(frame)
                if resampled is None:
                    continue
                for rf in resampled:
                    arr = rf.to_ndarray()                # (channels, samples)
                    if arr.size == 0:
                        continue
                    if arr.ndim == 2 and arr.shape[0] > 1:
                        arr = arr.transpose(1, 0)       # (samples, channels)
                    arr = arr.reshape(-1, self._channels)
                    # Throttle: wait if ring buffer is nearly full
                    while self._ring.filled > self._ring._cap * 0.8:
                        if not self._running:
                            return
                        time.sleep(0.002)
                    self._ring.write(arr)
        finally:
            container.close()
