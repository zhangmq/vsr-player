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


# ── Shaders ──

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

# Fullscreen quad: position (x,y) + texcoord (u,v) interleaved
_QUAD = np.array([
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

        # Unregister old PBO, delete, recreate
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
        """Upload a GPU RGBA uint8 tensor (H,W,4) to GL texture via CUDA-GL PBO."""
        if rgba_gpu.shape[1] != self._tex_w or rgba_gpu.shape[0] != self._tex_h:
            self.resize_texture(rgba_gpu.shape[1], rgba_gpu.shape[0])

        # Map PBO → get CUDA device pointer
        dev_ptr, size = cuda_gl_map(self._cu_resource)
        assert size >= self._pbo_size, f"PBO too small: {size} < {self._pbo_size}"

        # Copy tensor data → PBO (device-to-device, zero host traffic)
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
