"""Application controller — main loop, adaptive scaling, pipeline coordination."""
import time
import glfw
from .decoder import Decoder
from .vsr_pipeline import VSRPipeline
from .renderer import Renderer
from .config import adaptive_scale, DEBOUNCE_MS, DEFAULT_FPS, QUALITY_MAP

from nvvfx import VideoSuperRes


def _effective_quality(scale: int, user_quality: str):
    """Return (QualityLevel, display_name) for the given scale.

    scale == 1  → same-resolution denoise (no upscale overhead)
    scale >  1  → user-chosen upscale mode (HIGH/ULTRA already
                   include built-in denoise + sharpening)
    """
    if scale == 1:
        return VideoSuperRes.QualityLevel.DENOISE_HIGH, "DENOISE_HIGH"
    return QUALITY_MAP[user_quality], user_quality


class App:
    def __init__(self, video_path: str, quality: str = "HIGH"):
        self._decoder = Decoder(video_path)
        self._user_quality = quality
        self._playing = True

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
        self._pending_scale = scale

        glfw.set_key_callback(self._renderer.window, self._on_key)
        self._renderer.set_resize_callback(self._on_resize)

    def _on_key(self, window, key, scancode, action, mods):
        if action != glfw.PRESS:
            return
        if key == glfw.KEY_SPACE:
            self._playing = not self._playing
        elif key in (glfw.KEY_Q, glfw.KEY_ESCAPE):
            glfw.set_window_should_close(window, True)

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
              f"{self._fps:.1f} fps")
        print(f"VSR: x{self._scale} {self._eff_name}")
        print("Keys: SPACE=pause, Q/ESC=quit")

        t_frame_start = time.perf_counter()

        while not self._renderer.should_close():
            glfw.poll_events()
            self._apply_scale_change()

            # Pause loop
            while not self._playing and not self._renderer.should_close():
                glfw.wait_events_timeout(0.1)

            # Decode (prefetch next frame for pipeline overlap)
            ret, frame = self._decoder.consume_prefetched()
            if not ret:
                break
            self._decoder.prefetch()

            # VSR pipeline
            rgba_gpu = self._pipeline.process_frame(frame)

            # Render (video only, no overlay)
            self._renderer.begin_frame()
            self._renderer.upload_texture(rgba_gpu)
            self._renderer.draw_quad()
            self._renderer.end_frame()

            # Frame pacing
            elapsed = time.perf_counter() - t_frame_start
            sleep_time = self._frame_delay - elapsed
            if sleep_time > 0.001:
                time.sleep(sleep_time)
            t_frame_start = time.perf_counter()

        self._decoder.release()
        self._renderer.destroy()
