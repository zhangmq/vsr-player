# VSR Player v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the VSR prototype to a proper player with async CUDA pipeline, adaptive scaling, and GPU-native OpenGL rendering.

**Architecture:** cv2 decode → async H2D → VSR (non_blocking, CUDA stream) → DLPack clone → CUDA-GL interop PBO → GL texture → GL fullscreen render + GL overlay UI.

**Tech Stack:** Python 3.12 (mamba), nvidia-vfx, PyTorch, OpenCV, GLFW, PyOpenGL, CUDA driver API (ctypes)

---

### Task 1: Install new dependencies

**Files:** none

- [ ] **Step 1: Install GLFW and PyOpenGL**

```bash
mamba run -n vsr-player pip install glfw PyOpenGL
```

- [ ] **Step 2: Verify imports**

```bash
mamba run -n vsr-player python -c "
import glfw; print('glfw:', glfw._glfw._name if hasattr(glfw, '_glfw') else 'ok')
from OpenGL import GL; print('PyOpenGL ok')
print('All deps ready')
"
```

- [ ] **Step 3: Commit**

```bash
git add -A && git commit -m "chore: install glfw and PyOpenGL for GPU-native rendering"
```

---

### Task 2: Create package structure and config module

**Files:**
- Create: `src/vsr_player/__init__.py`
- Create: `src/vsr_player/config.py`

- [ ] **Step 1: Create package directory and init**

```bash
mkdir -p src/vsr_player
```

Write `src/vsr_player/__init__.py`:
```python
"""VSR Player — real-time AI video super-resolution playback."""
```

- [ ] **Step 2: Write config.py**

```python
"""Constants and adaptive scale calculation."""

from nvvfx import VideoSuperRes

QUALITY_MAP = {
    "LOW": VideoSuperRes.QualityLevel.LOW,
    "MEDIUM": VideoSuperRes.QualityLevel.MEDIUM,
    "HIGH": VideoSuperRes.QualityLevel.HIGH,
    "ULTRA": VideoSuperRes.QualityLevel.ULTRA,
}

MIN_SCALE = 1
MAX_SCALE = 4
DEBOUNCE_MS = 500  # delay before VSR reload on resize
DEFAULT_FPS = 30.0
DEFAULT_QUALITY = "HIGH"


def adaptive_scale(in_w: int, in_h: int, win_w: int, win_h: int) -> int:
    """Compute the best integer upscale factor to fill the window."""
    if in_w <= 0 or in_h <= 0:
        return 1
    s = min(win_w // in_w, win_h // in_h)
    return max(MIN_SCALE, min(MAX_SCALE, s))
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add package structure and config module"
```

---

### Task 3: Implement decoder module

**Files:**
- Create: `src/vsr_player/decoder.py`

- [ ] **Step 1: Write decoder.py**

```python
"""Video decode wrapper — reads frames from a video file via OpenCV."""

import cv2
from typing import Optional, Tuple


class Decoder:
    """OpenCV-based video decoder with prefetch support."""

    def __init__(self, path: str):
        self._cap = cv2.VideoCapture(path)
        if not self._cap.isOpened():
            raise FileNotFoundError(f"cannot open video: {path}")
        self.width = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.height = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fps = self._cap.get(cv2.CAP_PROP_FPS)
        self.fps = float(fps) if fps > 0 else 30.0
        self.frame_count = int(self._cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self._next_frame: Optional[Tuple[bool, object]] = None

    def read(self):
        """Read next frame. Returns (True, frame) or (False, None)."""
        ret, frame = self._cap.read()
        return ret, frame if ret else None

    def prefetch(self):
        """Read the next frame ahead of time (for pipeline overlap)."""
        if self._next_frame is None:
            ret, frame = self._cap.read()
            self._next_frame = (ret, frame if ret else None)

    def consume_prefetched(self):
        """Return the prefetched frame and clear the slot."""
        if self._next_frame is None:
            return self.read()
        ret_frame = self._next_frame
        self._next_frame = None
        ret, frame = ret_frame
        return ret, frame

    def seek(self, pos_frames: int):
        """Seek to a frame position."""
        self._cap.set(cv2.CAP_PROP_POS_FRAMES, pos_frames)
        self._next_frame = None

    def release(self):
        self._cap.release()

    def __del__(self):
        self.release()
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/decoder.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add decoder module with prefetch support"
```

---

### Task 4: Implement VSR pipeline module

**Files:**
- Create: `src/vsr_player/vsr_pipeline.py`

- [ ] **Step 1: Write vsr_pipeline.py**

```python
"""Async VSR pipeline — GPU format conversion and VSR effect with CUDA streams."""

import torch
from nvvfx import VideoSuperRes


def frame_to_gpu(bgr_uint8):
    """OpenCV BGR uint8 (H,W,3) → GPU RGB float32 (3,H,W) in [0,1]. Async H2D."""
    tensor = torch.from_numpy(bgr_uint8).cuda(non_blocking=True)
    tensor = tensor[..., [2, 1, 0]]          # BGR → RGB
    tensor = tensor.float().mul_(1.0 / 255.0)
    tensor = tensor.permute(2, 0, 1)         # HWC → CHW
    return tensor.contiguous()


def gpu_to_texture(rgb_float32):
    """GPU float32 RGB (3,H,W) [0,1] → GPU uint8 RGBA (H,W,4) for GL texture."""
    tensor = rgb_float32.permute(1, 2, 0)    # CHW → HWC
    tensor = tensor.mul_(255.0).clamp_(0, 255).to(torch.uint8)
    out_h, out_w = tensor.shape[:2]
    rgba = torch.zeros(out_h, out_w, 4, device=tensor.device, dtype=torch.uint8)
    rgba[..., :3] = tensor                    # RGB → RGBA (alpha=255)
    rgba[..., 3] = 255
    return rgba


class VSRPipeline:
    """Manages a VideoSuperRes instance with async CUDA stream processing."""

    def __init__(self, in_w: int, in_h: int, out_w: int, out_h: int,
                 quality=VideoSuperRes.QualityLevel.HIGH):
        self.vsr = VideoSuperRes(quality=quality)
        self.vsr.output_width = out_w
        self.vsr.output_height = out_h
        self.vsr.load()
        self.stream = torch.cuda.Stream()
        self.in_w, self.in_h = in_w, in_h
        self.out_w, self.out_h = out_w, out_h
        self.quality = quality

    def run_async(self, gpu_input):
        """Submit VSR on the managed CUDA stream, non-blocking."""
        with torch.cuda.stream(self.stream):
            result = self.vsr.run(gpu_input, non_blocking=True,
                                  stream_ptr=self.stream.cuda_stream)
            return result

    def sync_and_clone(self, result):
        """Synchronize the stream and clone the DLPack output."""
        self.stream.synchronize()
        return torch.from_dlpack(result.image).clone()

    def reload(self, out_w: int, out_h: int, quality=None):
        """Reconfigure output resolution and reload the VSR model."""
        if quality is not None:
            self.quality = quality
        self.out_w, self.out_h = out_w, out_h
        self.vsr.output_width = out_w
        self.vsr.output_height = out_h
        self.vsr.load()

    def process_frame(self, bgr_uint8):
        """Full per-frame pipeline: BGR numpy → VSR → RGBA GPU tensor."""
        gpu_in = frame_to_gpu(bgr_uint8)
        result = self.run_async(gpu_in)
        gpu_out = self.sync_and_clone(result)
        rgba = gpu_to_texture(gpu_out)
        return rgba
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/vsr_pipeline.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add async VSR pipeline with CUDA stream support"
```

---

### Task 5: Implement renderer module (GLFW + OpenGL + CUDA-GL interop)

**Files:**
- Create: `src/vsr_player/renderer.py`

- [ ] **Step 1: Write CUDA-GL interop helper**

```python
"""GPU-native GL renderer — GLFW window + OpenGL texture + CUDA-GL interop PBO."""
import ctypes
from ctypes import c_void_p, c_int, c_size_t, c_uint, byref, pointer, POINTER
import glfw
from OpenGL import GL
from OpenGL.GL import shaders
import numpy as np

# ── CUDA driver API (libcuda.so) ──

_libcuda = ctypes.CDLL("libcuda.so")

_cu_check = _libcuda.cuGetErrorString
_cu_check.argtypes = [c_int, POINTER(ctypes.c_char_p)]


def _check(ret: int):
    if ret != 0:
        msg = ctypes.c_char_p()
        _libcuda.cuGetErrorString(ret, byref(msg))
        raise RuntimeError(f"CUDA error {ret}: {msg.value.decode() if msg.value else 'unknown'}")


def cuda_gl_register_buffer(gl_buffer: int) -> c_void_p:
    """Register a GL buffer object with CUDA for interop."""
    resource = c_void_p()
    _check(_libcuda.cuGraphicsGLRegisterBuffer(
        byref(resource), c_uint(gl_buffer), 0x1  # WRITE_DISCARD
    ))
    return resource


def cuda_gl_map(resource: c_void_p):
    """Map a registered GL buffer, returning (device_ptr, size_bytes)."""
    _check(_libcuda.cuGraphicsMapResources(1, byref(resource), c_void_p(0)))
    dev_ptr = c_size_t()
    size = c_size_t()
    _check(_libcuda.cuGraphicsResourceGetMappedPointer(byref(dev_ptr), byref(size), resource))
    return dev_ptr.value, size.value


def cuda_gl_unmap(resource: c_void_p):
    _check(_libcuda.cuGraphicsUnmapResources(1, byref(resource), c_void_p(0)))


def cuda_gl_unregister(resource: c_void_p):
    _check(_libcuda.cuGraphicsUnregisterResource(resource))


def cuda_memcpy_dtod(dst_ptr: int, src_ptr: int, size: int):
    _check(_libcuda.cuMemcpyDtoD(c_void_p(dst_ptr), c_void_p(src_ptr), c_size_t(size)))
```

- [ ] **Step 2: Write vertex/fragment shaders and renderer class**

```python
_VERT_SHADER = """
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
"""

_FRAG_SHADER = """
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
void main() {
    FragColor = texture(uTexture, vTexCoord);
}
"""

# Fullscreen quad: position + texcoord
_QUAD = np.array([
    # x,    y,    u,   v
    -1.0, -1.0,  0.0, 0.0,
     1.0, -1.0,  1.0, 0.0,
     1.0,  1.0,  1.0, 1.0,
    -1.0, -1.0,  0.0, 0.0,
     1.0,  1.0,  1.0, 1.0,
    -1.0,  1.0,  0.0, 1.0,
], dtype=np.float32)


class Renderer:
    """GLFW + OpenGL fullscreen renderer with CUDA-GL interop PBO."""

    def __init__(self, width: int, height: int, title: str = "VSR Player"):
        if not glfw.init():
            raise RuntimeError("GLFW init failed")
        glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
        glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
        glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_COMPAT_PROFILE)
        glfw.window_hint(glfw.RESIZABLE, glfw.TRUE)

        monitor = glfw.get_primary_monitor()
        mode = glfw.get_video_mode(monitor)
        self._window = glfw.create_window(
            mode.size.width, mode.size.height, title, monitor, None
        )
        if not self._window:
            glfw.terminate()
            raise RuntimeError("GLFW window creation failed")

        glfw.make_context_current(self._window)
        glfw.swap_interval(1)  # vsync

        self._win_w = mode.size.width
        self._win_h = mode.size.height
        self._tex_w = width
        self._tex_h = height
        self._title = title
        self._on_resize = None  # callback(win_w, win_h)

        glfw.set_window_size_callback(self._window, self._on_window_resize)

        # Compile shaders
        vs = shaders.compileShader(_VERT_SHADER, GL.GL_VERTEX_SHADER)
        fs = shaders.compileShader(_FRAG_SHADER, GL.GL_FRAGMENT_SHADER)
        self._program = shaders.compileProgram(vs, fs)
        GL.glUseProgram(self._program)

        # VAO + VBO
        self._vao = GL.glGenVertexArrays(1)
        GL.glBindVertexArray(self._vao)
        self._vbo = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self._vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, _QUAD.nbytes, _QUAD, GL.GL_STATIC_DRAW)
        GL.glVertexAttribPointer(0, 2, GL.GL_FLOAT, GL.GL_FALSE, 16, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(1, 2, GL.GL_FLOAT, GL.GL_FALSE, 16, ctypes.c_void_p(8))
        GL.glEnableVertexAttribArray(1)

        # Texture
        self._texture = GL.glGenTextures(1)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        width, height, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)

        # PBO for CUDA-GL interop
        self._pbo_size = width * height * 4
        self._pbo = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo)
        GL.glBufferData(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_size, None, GL.GL_DYNAMIC_DRAW)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)

        self._cu_resource = cuda_gl_register_buffer(self._pbo)

        GL.glBindVertexArray(0)

    def _on_window_resize(self, window, w, h):
        self._win_w, self._win_h = w, h
        GL.glViewport(0, 0, w, h)
        if self._on_resize:
            self._on_resize(w, h)

    def set_resize_callback(self, cb):
        self._on_resize = cb

    def resize_texture(self, width: int, height: int):
        """Resize the GL texture and PBO for new VSR output dimensions."""
        self._tex_w, self._tex_h = width, height
        self._pbo_size = width * height * 4

        # Unregister old PBO, create new
        cuda_gl_unregister(self._cu_resource)
        GL.glDeleteBuffers(1, [self._pbo])

        # New texture
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        width, height, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)

        # New PBO
        self._pbo = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo)
        GL.glBufferData(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_size, None, GL.GL_DYNAMIC_DRAW)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)

        self._cu_resource = cuda_gl_register_buffer(self._pbo)

    def upload_texture(self, rgba_gpu):
        """Upload a GPU RGBA uint8 tensor to the GL texture via CUDA-GL PBO.
        
        Args:
            rgba_gpu: torch.uint8 tensor (H, W, 4) on CUDA
        """
        if rgba_gpu.shape[1] != self._tex_w or rgba_gpu.shape[0] != self._tex_h:
            self.resize_texture(rgba_gpu.shape[1], rgba_gpu.shape[0])

        # Map PBO → get CUDA device pointer
        dev_ptr, size = cuda_gl_map(self._cu_resource)
        assert size >= self._pbo_size, f"PBO too small: {size} < {self._pbo_size}"

        # Copy tensor data → PBO (device to device)
        tensor_ptr = rgba_gpu.data_ptr()
        cuda_memcpy_dtod(dev_ptr, tensor_ptr, self._pbo_size)
        cuda_gl_unmap(self._cu_resource)

        # PBO → texture
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo)
        GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0,
                           self._tex_w, self._tex_h,
                           GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, ctypes.c_void_p(0))
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)

    def begin_frame(self):
        """Clear and prepare for a new frame."""
        GL.glClear(GL.GL_COLOR_BUFFER_BIT)

    def draw_quad(self):
        """Draw the fullscreen textured quad."""
        GL.glUseProgram(self._program)
        GL.glBindVertexArray(self._vao)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
        GL.glDrawArrays(GL.GL_TRIANGLES, 0, 6)

    def end_frame(self):
        GL.glBindVertexArray(0)
        glfw.swap_buffers(self._window)

    def should_close(self) -> bool:
        return glfw.window_should_close(self._window)

    @property
    def window_size(self):
        return self._win_w, self._win_h

    @property
    def window(self):
        return self._window

    def destroy(self):
        cuda_gl_unregister(self._cu_resource)
        GL.glDeleteTextures(1, [self._texture])
        GL.glDeleteBuffers(1, [self._pbo])
        GL.glDeleteBuffers(1, [self._vbo])
        GL.glDeleteVertexArrays(1, [self._vao])
        GL.glDeleteProgram(self._program)
        glfw.destroy_window(self._window)
        glfw.terminate()
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/renderer.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add GL renderer with CUDA-GL interop PBO"
```

---

### Task 6: Implement overlay module

**Files:**
- Create: `src/vsr_player/overlay.py`

- [ ] **Step 1: Write overlay.py**

```python
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
        # Button rects in normalized NDC coords [-1, 1]
        self._btn_playpause = (0.0, 0.0, 0.0, 0.0)  # x1, y1, x2, y2
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
        bar_y2 = 1.0

        # Semi-transparent bar
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
        GL.glColor4f(0.08, 0.08, 0.08, 0.65)
        GL.glRectf(-1.0, bar_y1, 1.0, bar_y2)

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
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/overlay.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add GL overlay UI module"
```

---

### Task 7: Implement app module (main loop + adaptive scale)

**Files:**
- Create: `src/vsr_player/app.py`

- [ ] **Step 1: Write app.py**

```python
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
        # We need the window first, so use a placeholder; real init happens in run()
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

        # Keyboard: space, q, esc, f
        glfw.set_key_callback(self._renderer.window, self._on_key)

        # Mouse: left click for overlay buttons
        glfw.set_mouse_button_callback(self._renderer.window, self._on_mouse)

        # Resize
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
        print(f"VSR scale changed to ×{self._scale}  "
              f"({in_w}x{in_h} → {out_w}x{out_h})")

    def run(self):
        print(f"Playing: {self._decoder.width}x{self._decoder.height}, "
              f"{self._fps:.1f} fps")
        print(f"VSR: ×{self._scale} ({self._quality_name})")
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
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/app.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add app controller with adaptive scale and main loop"
```

---

### Task 8: Implement entry point

**Files:**
- Create: `src/vsr_player/__main__.py`

- [ ] **Step 1: Write __main__.py**

```python
"""Entry point: python -m vsr_player <video_file> [--quality QUALITY]."""
import argparse
import sys
from .app import App


def main():
    parser = argparse.ArgumentParser(
        description="VSR Player — real-time AI video super-resolution playback"
    )
    parser.add_argument("video_file", help="Path to input video file")
    parser.add_argument("--quality", default="HIGH",
                        choices=["LOW", "MEDIUM", "HIGH", "ULTRA"],
                        help="VSR quality level (default: HIGH)")
    args = parser.parse_args()

    try:
        app = App(args.video_file, args.quality)
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    except RuntimeError as e:
        print(f"Error: {e}")
        sys.exit(1)

    app.run()


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify syntax**

```bash
mamba run -n vsr-player python -c "import ast; ast.parse(open('src/vsr_player/__main__.py').read()); print('OK')"
```

- [ ] **Step 3: Commit**

```bash
git add src/ && git commit -m "feat: add CLI entry point"
```

---

### Task 9: Final integration — run, fix, update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md` — update run instructions
- Remove: `prototype.py` (or archive)

- [ ] **Step 1: Update CLAUDE.md run instructions**

Replace the "Build / Run / Test" section:

```markdown
## Build / Run / Test

```bash
# Activate environment
mamba activate vsr-player

# Install dependencies (once)
pip install torch opencv-python nvidia-vfx glfw PyOpenGL

# Run player (scale auto-adapted from window size)
python -m vsr_player <video_file> [--quality HIGH]

# Controls
#   SPACE = play/pause
#   F     = toggle fullscreen
#   Q/ESC = quit
#   Click overlay bar buttons

# Lint
ruff check .

# Run tests (when test suite exists)
pytest
```
```

- [ ] **Step 2: Archive old prototype**

```bash
mkdir -p archive && mv prototype.py archive/prototype-v1.py
```

- [ ] **Step 3: Run syntax check on all modules**

```bash
mamba run -n vsr-player python -c "
import ast
files = [
    'src/vsr_player/__init__.py',
    'src/vsr_player/__main__.py',
    'src/vsr_player/config.py',
    'src/vsr_player/decoder.py',
    'src/vsr_player/vsr_pipeline.py',
    'src/vsr_player/renderer.py',
    'src/vsr_player/overlay.py',
    'src/vsr_player/app.py',
]
for f in files:
    ast.parse(open(f).read())
    print(f'{f}: OK')
print('All modules pass syntax check')
"
```

- [ ] **Step 4: Dry-run CLI help**

```bash
mamba run -n vsr-player python -m vsr_player --help
```

Expected: argparse help text with `video_file` and `--quality` arguments.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat: final integration — update CLI, archive prototype, update docs"
```
