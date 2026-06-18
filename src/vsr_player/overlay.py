"""GL-drawn overlay UI — control bar, buttons, status text via cv2+GL."""
import numpy as np
import cv2
from OpenGL import GL
import ctypes

# Button layout constants
BAR_H = 80
BTN_W, BTN_H = 60, 40
BTN_MARGIN = 20
TEXT_W, TEXT_H = 512, 64  # text label texture size


class Overlay:
    """Draws a semi-transparent control bar with buttons and text labels."""

    def __init__(self):
        self._playing = True
        self._btn_playpause = (0.0, 0.0, 0.0, 0.0)
        self._btn_exit = (0.0, 0.0, 0.0, 0.0)
        self._on_playpause = None
        self._on_exit = None
        # Text texture
        self._text_tex = GL.glGenTextures(1)
        self._text_img = np.zeros((TEXT_H, TEXT_W, 4), dtype=np.uint8)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._text_tex)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        TEXT_W, TEXT_H, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)

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

    def _render_text_image(self, quality_name: str, scale: int,
                           fps: float, tex_w: int, tex_h: int):
        """Render status text to a cv2 RGBA image for GL texture upload."""
        img = np.zeros((TEXT_H, TEXT_W, 4), dtype=np.uint8)
        img[..., 3] = 255  # opaque background for text
        # White text on transparent bg
        text = f"x{scale}  {quality_name}    {fps:.1f} fps    {tex_w}x{tex_h}"
        (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 2)
        x = 4
        y = (TEXT_H + th) // 2
        cv2.putText(img, text, (x, y), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (220, 220, 220, 255), 2, cv2.LINE_AA)
        self._text_img = img

    def draw_bar(self, win_w: int, win_h: int, quality_name: str,
                 scale: int, fps: float, tex_w: int, tex_h: int):
        """Draw the overlay bar + buttons + status text."""
        GL.glUseProgram(0)
        GL.glMatrixMode(GL.GL_PROJECTION)
        GL.glLoadIdentity()
        GL.glMatrixMode(GL.GL_MODELVIEW)
        GL.glLoadIdentity()

        # ── Bar rect in NDC ──
        bar_y1 = -1.0 + (2.0 * BAR_H / win_h)

        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)

        # Semi-transparent bar background
        GL.glColor4f(0.08, 0.08, 0.08, 0.65)
        GL.glRectf(-1.0, bar_y1, 1.0, 1.0)

        # ── Play/pause button ──
        btn_x1 = -1.0 + (2.0 * BTN_MARGIN / win_w)
        btn_x2 = btn_x1 + (2.0 * BTN_W / win_w)
        btn_y1 = bar_y1 + (2.0 * (BAR_H - BTN_H) / (2.0 * win_h))
        btn_y2 = btn_y1 + (2.0 * BTN_H / win_h)
        GL.glColor4f(0.27, 0.51, 0.71, 1.0)
        GL.glRectf(btn_x1, btn_y1, btn_x2, btn_y2)
        self._btn_playpause = (btn_x1, btn_y1, btn_x2, btn_y2)

        # ── Exit button ──
        exit_x2 = 1.0 - (2.0 * BTN_MARGIN / win_w)
        exit_x1 = exit_x2 - (2.0 * BTN_W / win_w)
        GL.glColor4f(0.20, 0.20, 0.20, 1.0)
        GL.glRectf(exit_x1, btn_y1, exit_x2, btn_y2)
        self._btn_exit = (exit_x1, btn_y1, exit_x2, btn_y2)

        GL.glDisable(GL.GL_BLEND)

        # ── Status text via cv2 bitmap → GL texture quad ──
        self._render_text_image(quality_name, scale, fps, tex_w, tex_h)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._text_tex)
        GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0, TEXT_W, TEXT_H,
                           GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, self._text_img)

        # Draw text quad in the bar area
        GL.glEnable(GL.GL_TEXTURE_2D)
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
        GL.glColor4f(1.0, 1.0, 1.0, 1.0)

        tx1 = btn_x2 + 0.02
        tx2 = min(tx1 + 0.55, exit_x1 - 0.02)
        ty2 = btn_y2
        ty1 = btn_y1

        GL.glBegin(GL.GL_QUADS)
        GL.glTexCoord2f(0.0, 0.0); GL.glVertex2f(tx1, ty1)
        GL.glTexCoord2f(1.0, 0.0); GL.glVertex2f(tx2, ty1)
        GL.glTexCoord2f(1.0, 1.0); GL.glVertex2f(tx2, ty2)
        GL.glTexCoord2f(0.0, 1.0); GL.glVertex2f(tx1, ty2)
        GL.glEnd()

        GL.glDisable(GL.GL_TEXTURE_2D)
        GL.glDisable(GL.GL_BLEND)

    @property
    def btn_playpause(self):
        return self._btn_playpause

    @property
    def btn_exit(self):
        return self._btn_exit

    def destroy(self):
        GL.glDeleteTextures(1, [self._text_tex])
