"""Audio playback with sounddevice + ring buffer, providing the master clock.

Decodes audio in a background thread, feeds PCM into a ring buffer consumed
by a sounddevice OutputStream callback.  The audio clock (elapsed stream time)
serves as the master clock for A/V sync — matching the mpv/VLC model.

Supports pause/resume (tied to video pause) and seek (future scrub/seekbar).
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
        self._read = 0
        self._write = 0
        self._filled = 0
        self._lock = threading.Lock()

    def write(self, data: np.ndarray) -> int:
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

    def read(self, dst: np.ndarray) -> int:
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

    def clear(self):
        with self._lock:
            self._read = 0
            self._write = 0
            self._filled = 0

    @property
    def filled(self) -> int:
        return self._filled


class AudioPlayer:
    """Audio decoder + sounddevice output with pause/resume/seek.

    Lifecycle::

        ap = AudioPlayer("video.mp4")
        ap.start()       # begin playback
        ap.pause()       # pause (SPACE)
        ap.resume()      # resume
        ap.seek(10.0)    # jump to 10s (future)
        ap.stop()        # shutdown
    """

    def __init__(self, path: str, buffer_sec: float = 0.5):
        self._path = path
        self._sample_rate = 48000
        self._channels = 2
        self._ring: _RingBuffer | None = None
        self._stream: sd.OutputStream | None = None
        self._thread: threading.Thread | None = None
        self._running = False
        self._paused = False

        # Clock state
        self._start_time: float | None = None   # stream.time at last start/resume
        self._frozen_clock: float = 0.0          # clock value when paused

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
        """Master clock in seconds — elapsed stream time since start.

        Freezes at the current value when paused, so video frames will
        wait at the pause point rather than falling behind.
        """
        if self._stream is None or self._start_time is None:
            return 0.0
        if self._paused:
            return self._frozen_clock
        try:
            t = self._stream.time
            return (t - self._start_time) if t is not None else self._frozen_clock
        except Exception:
            return self._frozen_clock

    def start(self):
        """Begin audio playback (pre-buffers ~250ms)."""
        if self._ring is None:
            return
        self._running = True

        def _callback(outdata, frames, _time_info, _status):
            read = self._ring.read(outdata)
            if read < frames:
                outdata[read:] = 0.0

        self._thread = threading.Thread(target=self._decode_loop,
                                        args=(None,), daemon=True)
        self._thread.start()
        while self._ring.filled < self._sample_rate // 4:
            time.sleep(0.005)

        self._stream = sd.OutputStream(
            samplerate=self._sample_rate, channels=self._channels,
            dtype="float32", blocksize=1024, callback=_callback,
        )
        self._stream.start()
        self._start_time = self._stream.time

    def pause(self):
        """Pause audio output. Clock freezes. Thread keeps decoding."""
        if self._stream is None or self._paused:
            return
        self._frozen_clock = self.clock
        self._paused = True
        self._stream.stop()

    def resume(self):
        """Resume audio output. Clock continues from freeze point."""
        if self._stream is None or not self._paused:
            return
        self._stream.start()
        self._start_time = self._stream.time - self._frozen_clock
        self._paused = False

    def stop(self):
        """Shut down audio and join decode thread."""
        self._running = False
        if self._stream is not None:
            try:
                if not self._paused:
                    self._stream.stop()
            except Exception:
                pass
            self._stream.close()
            self._stream = None
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def seek(self, target_sec: float):
        """Seek audio to *target_sec* and restart decode from that position.

        Clears the ring buffer, restarts the decode thread at the new
        position, and resets the clock so it aligns with the new timeline.
        """
        was_paused = self._paused
        # Stop stream and decode thread
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        if self._stream is not None:
            if not self._paused:
                self._stream.stop()
            self._stream.close()
            self._stream = None
        self._ring.clear()
        self._paused = False
        self._frozen_clock = 0.0

        # Restart everything at the new position
        self._running = True
        self._thread = threading.Thread(target=self._decode_loop,
                                        args=(target_sec,), daemon=True)
        self._thread.start()
        while self._ring.filled < self._sample_rate // 4:
            time.sleep(0.005)

        def _callback(outdata, frames, _time_info, _status):
            read = self._ring.read(outdata)
            if read < frames:
                outdata[read:] = 0.0

        self._stream = sd.OutputStream(
            samplerate=self._sample_rate, channels=self._channels,
            dtype="float32", blocksize=1024, callback=_callback,
        )
        self._stream.start()
        # Align: clock should read target_sec after restart, not 0
        self._start_time = self._stream.time - target_sec

        if was_paused:
            self.pause()

    # ── Decode thread ───────────────────────────────────────────────

    def _decode_loop(self, seek_target: float | None = None):
        """Decode audio frames into the ring buffer.

        If *seek_target* is given, seek to that position in seconds before
        decoding.
        """
        container = av.open(self._path)
        try:
            a_stream = None
            for s in container.streams:
                if s.type == "audio":
                    a_stream = s
                    break
            if a_stream is None:
                return

            if seek_target is not None:
                ts = a_stream.time_base
                target_ts = int(seek_target / ts) if ts else int(seek_target * 1_000_000)
                container.seek(target_ts, stream=a_stream)

            for frame in container.decode(a_stream):
                if not self._running:
                    break
                resampled = self._resampler.resample(frame)
                if resampled is None:
                    continue
                for rf in resampled:
                    arr = rf.to_ndarray()
                    if arr.size == 0:
                        continue
                    if arr.ndim == 2 and arr.shape[0] > 1:
                        arr = arr.transpose(1, 0)
                    arr = arr.reshape(-1, self._channels)
                    while self._ring.filled > self._ring._cap * 0.8:
                        if not self._running:
                            return
                        time.sleep(0.002)
                    self._ring.write(arr)
        finally:
            container.close()
