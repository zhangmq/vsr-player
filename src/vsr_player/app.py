"""Application controller — main loop, adaptive scaling, pipeline coordination."""
import time
import glfw
from .decoder import Decoder
from .vsr_pipeline import VSRPipeline
from .renderer import Renderer
from .overlay import Overlay
from .config import adaptive_scale, DEBOUNCE_MS, DEFAULT_FPS, QUALITY_MAP


class App:
    def __init__(self, video_path: str, quality: str = "HIGH"):
        self._decoder = Decoder(video_path)
        quality_level = QUALITY_MAP[quality]

        # Initial scale: compute from fullscreen dimensions
        in_w, in_h = self._decoder.width, self._decoder.height
        self._renderer = Renderer(in_w, in_h)  # initial texture at input res
        win_w, win_h = self._renderer.window_size
        scale = adaptive_scale(in_w, in_h, win_w, win_h)

        out_w = in_w * scale
        out_h = in_h * scale
        self._pipeline = VSRPipeline(in_w, in_h, out_w, out_h, quality_level)
        self._renderer.resize_texture(out_w, out_h)

        self._scale = scale
        self._quality_name = quality
        self._fps = self._decoder.fps
        self._frame_delay = 1.0 / self._fps if self._fps > 0 else 1.0 / DEFAULT_FPS
        self._last_resize_time = 0.0
        self._pending_scale = scale

        # Overlay
        self._overlay = Overlay()
        self._overlay.set_playpause_callback(lambda: self._overlay.toggle())
        self._overlay.set_exit_callback(lambda: glfw.set_window_should_close(
            self._renderer.window, True))

        # Callbacks
        glfw.set_key_callback(self._renderer.window, self._on_key)
        glfw.set_mouse_button_callback(self._renderer.window, self._on_mouse)
        self._renderer.set_resize_callback(self._on_resize)

    def _on_key(self, window, key, scancode, action, mods):
        if action != glfw.PRESS:
            return
        if key == glfw.KEY_SPACE:
            self._overlay.toggle()
        elif key in (glfw.KEY_Q, glfw.KEY_ESCAPE):
            glfw.set_window_should_close(window, True)
        elif key == glfw.KEY_F:
            monitor = glfw.get_primary_monitor()
            mode = glfw.get_video_mode(monitor)
            glfw.set_window_monitor(window, monitor,
                                    0, 0, mode.size.width, mode.size.height,
                                    mode.refresh_rate)

    def _on_mouse(self, window, button, action, mods):
        if button == glfw.MOUSE_BUTTON_LEFT and action == glfw.PRESS:
            x, y = glfw.get_cursor_pos(window)
            self._overlay.hit_test(self._renderer.window_size[0],
                                   self._renderer.window_size[1],
                                   int(x), int(y))

    def _on_resize(self, win_w: int, win_h: int):
        in_w, in_h = self._decoder.width, self._decoder.height
        new_scale = adaptive_scale(in_w, in_h, win_w, win_h)
        if new_scale != self._pending_scale:
            self._pending_scale = new_scale
            self._last_resize_time = time.time()

    def _apply_scale_change(self):
        """Reload VSR with new scale if debounce period has elapsed."""
        if self._pending_scale == self._scale:
            return
        if time.time() - self._last_resize_time < DEBOUNCE_MS / 1000.0:
            return
        in_w, in_h = self._decoder.width, self._decoder.height
        out_w = in_w * self._pending_scale
        out_h = in_h * self._pending_scale
        self._pipeline.reload(out_w, out_h)
        self._renderer.resize_texture(out_w, out_h)
        self._scale = self._pending_scale
        print(f"VSR scale changed to x{self._scale}  "
              f"({in_w}x{in_h} -> {out_w}x{out_h})")

    def run(self):
        print(f"Playing: {self._decoder.width}x{self._decoder.height}, "
              f"{self._fps:.1f} fps")
        print(f"VSR: x{self._scale} ({self._quality_name})")
        print("Keys: SPACE=pause, F=fullscreen, Q/ESC=quit")
        print("Click overlay buttons for play/pause and exit.")

        t_frame_start = time.perf_counter()

        while not self._renderer.should_close():
            glfw.poll_events()
            self._apply_scale_change()

            # Pause loop
            while (not self._overlay.playing and
                   not self._renderer.should_close()):
                glfw.wait_events_timeout(0.1)

            # Decode
            ret, frame = self._decoder.read()
            if not ret:
                break

            # VSR pipeline
            rgba_gpu = self._pipeline.process_frame(frame)

            # Render
            self._renderer.begin_frame()
            self._renderer.upload_texture(rgba_gpu)
            self._renderer.draw_quad()
            win_w, win_h = self._renderer.window_size
            self._overlay.draw_bar(win_w, win_h, self._quality_name,
                                   self._scale, self._fps,
                                   self._pipeline.out_w, self._pipeline.out_h)
            self._renderer.end_frame()

            # Frame pacing
            elapsed = time.perf_counter() - t_frame_start
            sleep_time = self._frame_delay - elapsed
            if sleep_time > 0.001:
                time.sleep(sleep_time)
            t_frame_start = time.perf_counter()

        self._decoder.release()
        self._renderer.destroy()
