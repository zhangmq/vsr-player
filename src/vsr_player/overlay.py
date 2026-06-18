"""GL-drawn overlay UI — control bar, play/pause button, exit button, status."""
from OpenGL import GL
import glfw

# Button layout constants
BAR_H = 80
BTN_W, BTN_H = 60, 40
BTN_MARGIN = 20


class Overlay:
    """Draws a semi-transparent control bar with buttons at the bottom of the viewport.

    Mouse coordinates are in window space; hit-testing is done by the caller.
    """

    def __init__(self):
        self._playing = True
        self._btn_playpause = (0.0, 0.0, 0.0, 0.0)  # x1, y1, x2, y2 in NDC
        self._btn_exit = (0.0, 0.0, 0.0, 0.0)
        self._on_playpause = None
        self._on_exit = None

    @property
    def playing(self):
        return self._playing

    def toggle(self):
        self._playing = not self._playing
        return self._playing

    def set_playpause_callback(self, cb):
        self._on_playpause = cb

    def set_exit_callback(self, cb):
        self._on_exit = cb

    def hit_test(self, win_w: int, win_h: int, mx: int, my: int) -> bool:
        """Check if a click hits any button. Returns True if handled."""
        # Convert screen coords to NDC
        nx = (2.0 * mx / win_w) - 1.0
        ny = 1.0 - (2.0 * my / win_h)

        x1, y1, x2, y2 = self._btn_playpause
        if x1 <= nx <= x2 and y1 <= ny <= y2:
            if self._on_playpause:
                self._on_playpause()
            return True

        x1, y1, x2, y2 = self._btn_exit
        if x1 <= nx <= x2 and y1 <= ny <= y2:
            if self._on_exit:
                self._on_exit()
            return True

        return False

    def draw_bar(self, win_w: int, win_h: int, quality_name: str,
                 scale: int, fps: float, tex_w: int, tex_h: int):
        """Draw the overlay bar using immediate-mode GL. Called per frame."""
        GL.glUseProgram(0)
        GL.glMatrixMode(GL.GL_PROJECTION)
        GL.glLoadIdentity()
        GL.glMatrixMode(GL.GL_MODELVIEW)
        GL.glLoadIdentity()

        # Bar rect in NDC
        bar_y1 = -1.0 + (2.0 * BAR_H / win_h)

        # Semi-transparent bar
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
        GL.glColor4f(0.08, 0.08, 0.08, 0.65)
        GL.glRectf(-1.0, bar_y1, 1.0, 1.0)

        # Play/pause button
        btn_x1 = -1.0 + (2.0 * BTN_MARGIN / win_w)
        btn_x2 = btn_x1 + (2.0 * BTN_W / win_w)
        btn_y1 = bar_y1 + (2.0 * (BAR_H - BTN_H) / (2.0 * win_h))
        btn_y2 = btn_y1 + (2.0 * BTN_H / win_h)
        GL.glColor4f(0.27, 0.51, 0.71, 1.0)
        GL.glRectf(btn_x1, btn_y1, btn_x2, btn_y2)
        self._btn_playpause = (btn_x1, btn_y1, btn_x2, btn_y2)

        # Exit button
        exit_x2 = 1.0 - (2.0 * BTN_MARGIN / win_w)
        exit_x1 = exit_x2 - (2.0 * BTN_W / win_w)
        GL.glColor4f(0.20, 0.20, 0.20, 1.0)
        GL.glRectf(exit_x1, btn_y1, exit_x2, btn_y2)
        self._btn_exit = (exit_x1, btn_y1, exit_x2, btn_y2)

        GL.glDisable(GL.GL_BLEND)

    @property
    def btn_playpause(self):
        return self._btn_playpause

    @property
    def btn_exit(self):
        return self._btn_exit
