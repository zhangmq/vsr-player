"""Basic PCM audio playback via PipeWire/ALSA subprocess pipe.

Decodes audio in a background daemon thread and pipes s16le PCM to
``pw-play`` (PipeWire) or ``aplay`` (ALSA) for low-latency output.
"""

import subprocess
import threading

import av
import numpy as np


class AudioPlayer:
    """Background audio decoder and player.

    Finds the first audio stream in a media file, decodes PCM in a
    daemon thread, and sends s16le samples to a subprocess pipe.

    Usage::

        ap = AudioPlayer("video.mp4")
        ap.start()
        # ... video playback ...
        ap.stop()
    """

    def __init__(self, path: str):
        self._path = path
        self._proc = None
        self._thread = None
        self._running = False

        # Probe for audio stream
        container = av.open(path)
        self._audio_stream = None
        for s in container.streams:
            if s.type == "audio":
                self._audio_stream = s
                break

        if self._audio_stream is None:
            container.close()
            return

        ctx = self._audio_stream.codec_context
        self._sample_rate = ctx.sample_rate or 48000
        self._channels = ctx.channels or 2
        container.close()

    @property
    def is_active(self) -> bool:
        """True if the file contains an audio stream."""
        return self._audio_stream is not None

    def start(self):
        """Launch playback subprocess and decode thread."""
        if self._audio_stream is None:
            return

        self._proc = self._spawn_player()
        self._running = True
        self._thread = threading.Thread(target=self._decode_loop, daemon=True)
        self._thread.start()

    def _spawn_player(self) -> subprocess.Popen:
        """Spawn pw-play or aplay, returning the Popen handle."""
        # Try PipeWire first
        try:
            return subprocess.Popen(
                ["pw-play", "--rate", str(self._sample_rate),
                 "--channels", str(self._channels),
                 "--format", "s16le", "-"],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError:
            pass

        # Fall back to ALSA
        try:
            return subprocess.Popen(
                ["aplay", "-r", str(self._sample_rate),
                 "-c", str(self._channels),
                 "-f", "S16_LE", "-"],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError:
            raise RuntimeError("Neither pw-play nor aplay found for audio output")

    def _decode_loop(self):
        """Decode audio frames in a loop and write to the subprocess pipe."""
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
                # frame: planar float32, shape (channels, samples)
                arr = frame.to_ndarray()
                if arr.size == 0:
                    continue

                # Convert planar (C, N) to interleaved (N, C)
                if arr.ndim == 2 and arr.shape[0] > 1:
                    arr = arr.transpose(1, 0)

                # float32 [-1, 1] → s16le
                arr_int16 = (arr * 32767.0).clip(-32768, 32767).astype(np.int16)
                try:
                    self._proc.stdin.write(arr_int16.tobytes())
                except BrokenPipeError:
                    break
        finally:
            container.close()

    def stop(self):
        """Stop playback and clean up."""
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        if self._proc is not None:
            try:
                self._proc.stdin.close()
            except Exception:
                pass
            self._proc.wait(timeout=2.0)
