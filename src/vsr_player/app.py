"""Application controller — main loop, adaptive scaling, pipeline coordination."""
import time
import glfw
from .decoder import Decoder
from .vsr_pipeline import VSRPipeline
from .renderer import Renderer
from .config import adaptive_scale, DEBOUNCE_MS, DEFAULT_FPS, QUALITY_MAP, DENOISE_MAP
from .nv12_to_rgb import convert_frame_to_rgb
from .audio import AudioPlayer


def _effective_quality(scale: int, user_quality: str):
    """Return (QualityLevel, display_name) for the given scale.

    scale == 1  → same-resolution denoise matching the user's quality tier
    scale >  1  → user-chosen upscale mode (already includes denoise + sharpening)
    """
    if scale == 1:
        q = DENOISE_MAP[user_quality]
        return q, q.name
    return QUALITY_MAP[user_quality], user_quality


class App:
    def __init__(self, video_path: str, quality: str = "HIGH"):
        self._decoder = Decoder(video_path)
        self._user_quality = quality
        self._playing = True
        self._audio = AudioPlayer(video_path)

        # Initial scale: compute from window dimensions
        in_w, in_h = self._decoder.width, self._decoder.height
        self._renderer = Renderer(in_w, in_h)
        win_w, win_h = self._renderer.window_size
        scale = adaptive_scale(in_w, in_h, win_w, win_h)

        out_w = in_w * scale
        out_h = in_h * scale
        eff_quality, eff_name = _effective_quality(scale, quality)
        self._pipeline = VSRPipeline(in_w, in_h, out_w, out_h, eff_quality)
        self._renderer.resize_texture(out_w, out_h)

        self._scale = scale
        self._eff_name = eff_name
        self._fps = self._decoder.fps
        self._frame_delay = 1.0 / self._fps if self._fps > 0 else 1.0 / DEFAULT_FPS
        self._last_resize_time = 0.0
        self._skipped_frames = 0
        self._pending_scale = scale

        glfw.set_key_callback(self._renderer.window, self._on_key)
        self._renderer.set_resize_callback(self._on_resize)

    def _on_key(self, window, key, scancode, action, mods):
        if action != glfw.PRESS:
            return
        if key == glfw.KEY_SPACE:
            self._playing = not self._playing
            if self._playing:
                self._audio.resume()
            else:
                self._audio.pause()
        elif key == glfw.KEY_RIGHT:
            self._seek_relative(5.0)
        elif key == glfw.KEY_LEFT:
            self._seek_relative(-5.0)
        elif key in (glfw.KEY_Q, glfw.KEY_ESCAPE):
            glfw.set_window_should_close(window, True)

    def _seek_relative(self, delta_sec: float):
        """Seek video + audio by *delta_sec* from current position."""
        current = self._audio.clock if self._audio.is_active else 0.0
        target = max(0.0, current + delta_sec)
        self._perform_seek(target)

    def _perform_seek(self, target_sec: float):
        """Coordinated seek: audio + video to *target_sec*."""
        was_playing = self._playing
        # Pause both while seeking
        if self._audio.is_active:
            self._audio.pause()
        self._playing = False

        # Seek video
        self._decoder.seek_seconds(target_sec)

        # Seek audio (restarts stream + decode at new position)
        if self._audio.is_active:
            self._audio.seek(target_sec)

        # Resume
        self._playing = was_playing
        if self._playing and self._audio.is_active:
            self._audio.resume()

    def _on_resize(self, win_w: int, win_h: int):
        in_w, in_h = self._decoder.width, self._decoder.height
        new_scale = adaptive_scale(in_w, in_h, win_w, win_h)
        if new_scale != self._pending_scale:
            self._pending_scale = new_scale
            self._last_resize_time = time.time()

    def _apply_scale_change(self):
        if self._pending_scale == self._scale:
            return
        if time.time() - self._last_resize_time < DEBOUNCE_MS / 1000.0:
            return
        in_w, in_h = self._decoder.width, self._decoder.height
        out_w = in_w * self._pending_scale
        out_h = in_h * self._pending_scale
        eff_quality, eff_name = _effective_quality(self._pending_scale,
                                                   self._user_quality)
        self._pipeline.reconfigure(out_w, out_h, quality=eff_quality)
        self._renderer.resize_texture(out_w, out_h)
        self._scale = self._pending_scale
        self._eff_name = eff_name
        print(f"VSR: x{self._scale}  {eff_name}  "
              f"({in_w}x{in_h} -> {out_w}x{out_h})")

    def run(self):
        print(f"Playing: {self._decoder.width}x{self._decoder.height}, "
              f"{self._fps:.1f} fps"
              f"{' [NVDEC]' if self._decoder.is_hardware else ' [SW]'}")
        print(f"VSR: x{self._scale} {self._eff_name}")
        print("Keys: SPACE=pause, Q/ESC=quit")

        self._audio.start()

        while not self._renderer.should_close():
            glfw.poll_events()
            self._apply_scale_change()

            # Pause loop
            while not self._playing and not self._renderer.should_close():
                glfw.wait_events_timeout(0.1)

            # Decode
            ret, frame = self._decoder.consume_prefetched()
            if not ret:
                break

            # A/V sync — audio master clock (mpv/VLC model)
            if self._audio.is_active and frame.pts is not None:
                pts = float(frame.pts * self._decoder.time_base)
                audio_clock = self._audio.clock
                delay = pts - audio_clock

                if delay > 0.001:
                    # Video ahead — wait for audio to catch up
                    time.sleep(delay)
                elif delay < -self._frame_delay:
                    # Video behind by more than one frame — skip to catch up
                    self._skipped_frames += 1
                    continue
            else:
                # No audio — simple frame pacing
                time.sleep(self._frame_delay)

            # Convert PyAV frame → GPU float32 RGB (HW NV12 or SW RGB).
            # MUST happen before prefetch() — NVDEC reuses GPU buffers.
            rgb_gpu = convert_frame_to_rgb(frame, self._decoder.is_hardware)

            # Prefetch next frame (pipeline overlap)
            self._decoder.prefetch()

            # VSR pipeline (GPU tensor in, RGBA GPU tensor out)
            rgba_gpu = self._pipeline.process_gpu_frame(rgb_gpu)

            # Render (video only, no overlay)
            self._renderer.begin_frame()
            self._renderer.upload_texture(rgba_gpu)
            self._renderer.draw_quad()
            self._renderer.end_frame()

        self._audio.stop()
        self._decoder.release()
        self._renderer.destroy()
