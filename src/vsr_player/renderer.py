"""GPU-native GL renderer — GLFW window + OpenGL texture + CUDA-GL interop PBO.

Uses the CUDA *runtime* API (libcudart.so) — the same API that PyTorch uses
internally — so that context management is shared.  We select a CUDA device
that is compatible with the current OpenGL context via cudaGLGetDevices.
"""
import ctypes
from ctypes import (c_void_p, c_int, c_uint, c_size_t,
                    byref, pointer, POINTER, cast)
import glfw
from OpenGL import GL
from OpenGL.GL import shaders
import numpy as np

# ── CUDA runtime API (libcudart.so) — shared context with PyTorch ──

def _find_libcudart():
    """Locate libcudart.so on the system."""
    import glob as _glob, os as _os
    # nvidia-cuda-runtime wheels install into site-packages/nvidia/cu*/lib/
    candidates = []
    for pattern in [
        # pip-installed nvidia-cuXX wheels
        "/home/zmq/miniforge3/envs/vsr-player/lib/python*/site-packages/nvidia/cu*/lib/libcudart.so*",
        # system CUDA toolkit (Arch)
        "/opt/cuda/lib64/libcudart.so*",
        "/opt/cuda/targets/*/lib/libcudart.so*",
        # standard
        "/usr/lib/libcudart.so*",
        "/usr/local/cuda/lib64/libcudart.so*",
    ]:
        candidates.extend(sorted(_glob.glob(pattern)))
    # Prefer unversioned, then highest version
    unversioned = [p for p in candidates if p.endswith('.so')]
    if unversioned:
        return unversioned[0]
    if candidates:
        return candidates[-1]  # highest version number
    raise RuntimeError(
        "libcudart.so not found. Install the nvidia-cuda-runtime package "
        "or CUDA toolkit."
    )

_cudart = ctypes.CDLL(_find_libcudart())

# cudaError_t constants
_cudaSuccess = 0

# cudaMemcpyKind
_cudaMemcpyDeviceToDevice = 3

# cudaGLDeviceList
_cudaGLDeviceListAll = 3

# cudaGraphicsRegisterFlags
_cudaGraphicsRegisterFlagsWriteDiscard = 1


def _cuda_check(ret: int, fn_name: str = ""):
    if ret != _cudaSuccess:
        err_str = ctypes.c_char_p()
        _cudart.cudaGetErrorString(ret, byref(err_str))
        msg = err_str.value.decode() if err_str.value else "unknown"
        raise RuntimeError(f"CUDA error {ret} ({fn_name}): {msg}")


# Setup function signatures (cudaError_t return, defined as needed)

_cudart.cudaGetErrorString.argtypes = [c_int, POINTER(ctypes.c_char_p)]

# int cudaGLGetDevices(unsigned int *count, int *devices, unsigned int ndev,
#                      cudaGLDeviceList list)
_cudart.cudaGLGetDevices.argtypes = [POINTER(c_uint), POINTER(c_int),
                                     c_uint, c_uint]
_cudart.cudaGLGetDevices.restype = c_int

# cudaError_t cudaSetDevice(int device)
_cudart.cudaSetDevice.argtypes = [c_int]
_cudart.cudaSetDevice.restype = c_int

# cudaError_t cudaGraphicsGLRegisterBuffer(cudaGraphicsResource_t *res,
#                                          GLuint buf, unsigned int flags)
_cudart.cudaGraphicsGLRegisterBuffer.argtypes = [
    POINTER(c_void_p), c_uint, c_uint
]
_cudart.cudaGraphicsGLRegisterBuffer.restype = c_int

# cudaError_t cudaGraphicsMapResources(int count,
#                                      cudaGraphicsResource_t *res,
#                                      cudaStream_t stream)
_cudart.cudaGraphicsMapResources.argtypes = [c_int, POINTER(c_void_p), c_void_p]
_cudart.cudaGraphicsMapResources.restype = c_int

# cudaError_t cudaGraphicsResourceGetMappedPointer(void **devPtr,
#                                                  size_t *size,
#                                                  cudaGraphicsResource_t res)
_cudart.cudaGraphicsResourceGetMappedPointer.argtypes = [
    POINTER(c_void_p), POINTER(c_size_t), c_void_p
]
_cudart.cudaGraphicsResourceGetMappedPointer.restype = c_int

# cudaError_t cudaGraphicsUnmapResources(int count,
#                                        cudaGraphicsResource_t *res,
#                                        cudaStream_t stream)
_cudart.cudaGraphicsUnmapResources.argtypes = [c_int, POINTER(c_void_p), c_void_p]
_cudart.cudaGraphicsUnmapResources.restype = c_int

# cudaError_t cudaGraphicsUnregisterResource(cudaGraphicsResource_t res)
_cudart.cudaGraphicsUnregisterResource.argtypes = [c_void_p]
_cudart.cudaGraphicsUnregisterResource.restype = c_int

# cudaError_t cudaMemcpy(void *dst, const void *src, size_t count,
#                        cudaMemcpyKind kind)
_cudart.cudaMemcpy.argtypes = [c_void_p, c_void_p, c_size_t, c_int]
_cudart.cudaMemcpy.restype = c_int

# cudaError_t cudaDeviceSynchronize(void)
_cudart.cudaDeviceSynchronize.argtypes = []
_cudart.cudaDeviceSynchronize.restype = c_int


def _pick_gl_device() -> int:
    """Return a CUDA device index compatible with the current GL context."""
    count = c_uint(0)
    _cuda_check(_cudart.cudaGLGetDevices(byref(count), None, 0,
                                         _cudaGLDeviceListAll),
                "cudaGLGetDevices(count)")
    if count.value == 0:
        raise RuntimeError("No CUDA device supports the current OpenGL context")
    devices = (c_int * count.value)()
    _cuda_check(_cudart.cudaGLGetDevices(byref(count), devices, count.value,
                                         _cudaGLDeviceListAll),
                "cudaGLGetDevices(list)")
    return devices[0]


def _ensure_gl_device() -> int:
    """Set the CUDA device to the one compatible with the GL context."""
    dev = _pick_gl_device()
    _cuda_check(_cudart.cudaSetDevice(dev), "cudaSetDevice")
    _cuda_check(_cudart.cudaDeviceSynchronize(), "cudaDeviceSynchronize")
    return dev


def cuda_gl_register_buffer(gl_buffer: int) -> c_void_p:
    """Register a GL buffer object with CUDA for interop."""
    resource = c_void_p()
    _cuda_check(
        _cudart.cudaGraphicsGLRegisterBuffer(
            byref(resource), c_uint(gl_buffer),
            _cudaGraphicsRegisterFlagsWriteDiscard),
        "cudaGraphicsGLRegisterBuffer")
    return resource


def cuda_gl_map(resource: c_void_p):
    """Map a registered GL buffer, returning (device_ptr, size_bytes)."""
    _cuda_check(
        _cudart.cudaGraphicsMapResources(1, byref(resource), c_void_p(0)),
        "cudaGraphicsMapResources")
    dev_ptr = c_void_p()
    size = c_size_t()
    _cuda_check(
        _cudart.cudaGraphicsResourceGetMappedPointer(
            byref(dev_ptr), byref(size), resource),
        "cudaGraphicsResourceGetMappedPointer")
    return dev_ptr.value, size.value


def cuda_gl_unmap(resource: c_void_p):
    _cuda_check(
        _cudart.cudaGraphicsUnmapResources(1, byref(resource), c_void_p(0)),
        "cudaGraphicsUnmapResources")


def cuda_gl_unregister(resource: c_void_p):
    _cuda_check(
        _cudart.cudaGraphicsUnregisterResource(resource),
        "cudaGraphicsUnregisterResource")


def cuda_memcpy_dtod(dst_ptr: int, src_ptr: int, size: int):
    _cuda_check(
        _cudart.cudaMemcpy(c_void_p(dst_ptr), c_void_p(src_ptr),
                           c_size_t(size), _cudaMemcpyDeviceToDevice),
        "cudaMemcpy D2D")


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
    FragColor = texture(uTexture, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
}
"""

_FRAG_COMPARE = """
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTextureOrig;
uniform sampler2D uTextureVSR;
void main() {
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    if (vTexCoord.x < 0.5) {
        FragColor = texture(uTextureOrig, uv);
    } else {
        FragColor = texture(uTextureVSR, uv);
    }
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
        glfw.window_hint(glfw.DECORATED, glfw.FALSE)

        # Create a borderless window at the primary-monitor resolution, but
        # without forcing a specific monitor.  On tiling Wayland compositors
        # (niri, Hyprland, Sway) this lets the compositor place the window
        # on the currently-active monitor instead of the primary one.
        monitor = glfw.get_primary_monitor()
        mode = glfw.get_video_mode(monitor)
        self._window = glfw.create_window(
            mode.size.width, mode.size.height, title, None, None
        )
        if not self._window:
            glfw.terminate()
            raise RuntimeError("GLFW window creation failed")

        glfw.make_context_current(self._window)
        glfw.swap_interval(1)  # vsync

        # Select the CUDA device that can interop with this GL context.
        # Uses the CUDA *runtime* API — same API PyTorch uses — so the
        # context is shared automatically.  No manual push/pop needed.
        import torch
        assert torch.cuda.is_available(), "CUDA GPU required"
        self._cu_device = _ensure_gl_device()
        print(f"CUDA-GL interop: using device {self._cu_device} "
              f"({torch.cuda.get_device_name(self._cu_device)})")

        self._tex_w = width
        self._tex_h = height
        self._on_resize = None  # callback(win_w, win_h)

        glfw.set_window_size_callback(self._window, self._on_window_resize)
        glfw.set_framebuffer_size_callback(self._window, self._on_fb_resize)

        # Use framebuffer size (physical pixels) for OpenGL — not window
        # size (logical pixels on HiDPI / scaled Wayland desktops).
        self._win_w, self._win_h = glfw.get_framebuffer_size(self._window)
        self._update_viewport()

        # Compile shaders
        vs = shaders.compileShader(_VERT_SHADER, GL.GL_VERTEX_SHADER)

        fs_normal = shaders.compileShader(_FRAG_SHADER, GL.GL_FRAGMENT_SHADER)
        self._program = shaders.compileProgram(vs, fs_normal)

        fs_compare = shaders.compileShader(_FRAG_COMPARE, GL.GL_FRAGMENT_SHADER)
        self._program_compare = shaders.compileProgram(vs, fs_compare)
        self._uloc_orig = GL.glGetUniformLocation(self._program_compare, "uTextureOrig")
        self._uloc_vsr = GL.glGetUniformLocation(self._program_compare, "uTextureVSR")

        self._compare_mode = False
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

        # Second texture + PBO for original (pre-VSR) frame in compare mode
        self._tex_orig = GL.glGenTextures(1)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex_orig)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        width, height, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)

        self._pbo_orig = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_orig)
        GL.glBufferData(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_size, None, GL.GL_DYNAMIC_DRAW)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)
        self._cu_resource_orig = cuda_gl_register_buffer(self._pbo_orig)

        GL.glBindVertexArray(0)

    def _update_viewport(self):
        """Compute a centred, aspect-ratio-preserving viewport.

        The VSR output (`_tex_w` × `_tex_h`) is mapped to the largest centred
        rectangle that fits inside the window without stretching or cropping.
        Black bars (letterbox / pillarbox) fill any remaining area.
        """
        tex_aspect = self._tex_w / self._tex_h
        win_aspect = self._win_w / self._win_h
        if win_aspect > tex_aspect:
            # Window is wider → pillarbox
            vp_h = self._win_h
            vp_w = int(self._win_h * tex_aspect)
        else:
            # Window is taller → letterbox
            vp_w = self._win_w
            vp_h = int(self._win_w / tex_aspect)
        self._vp_x = (self._win_w - vp_w) // 2
        self._vp_y = (self._win_h - vp_h) // 2
        self._vp_w = vp_w
        self._vp_h = vp_h

    def _on_fb_resize(self, window, w, h):
        """Framebuffer resize — physical pixels, correct for HiDPI."""
        self._win_w, self._win_h = w, h
        self._update_viewport()
        if self._on_resize:
            self._on_resize(w, h)

    def _on_window_resize(self, window, w, h):
        """Window (logical) resize — forwarded for completeness."""
        pass  # framebuffer callback above handles the real work

    def set_resize_callback(self, cb):
        self._on_resize = cb

    def resize_texture(self, width: int, height: int):
        """Resize GL textures and PBOs for new VSR output dimensions."""
        self._tex_w, self._tex_h = width, height
        self._pbo_size = width * height * 4

        # VSR texture + PBO
        cuda_gl_unregister(self._cu_resource)
        GL.glDeleteBuffers(1, [self._pbo])
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        width, height, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)
        self._pbo = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo)
        GL.glBufferData(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_size, None, GL.GL_DYNAMIC_DRAW)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)
        self._cu_resource = cuda_gl_register_buffer(self._pbo)

        # Original texture + PBO
        cuda_gl_unregister(self._cu_resource_orig)
        GL.glDeleteBuffers(1, [self._pbo_orig])
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex_orig)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8,
                        width, height, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)
        self._pbo_orig = GL.glGenBuffers(1)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_orig)
        GL.glBufferData(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_size, None, GL.GL_DYNAMIC_DRAW)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)
        self._cu_resource_orig = cuda_gl_register_buffer(self._pbo_orig)

        self._update_viewport()

    def upload_texture(self, rgba_gpu):
        """Upload a GPU RGBA uint8 tensor (H,W,4) to GL texture via CUDA-GL PBO."""
        if rgba_gpu.shape[1] != self._tex_w or rgba_gpu.shape[0] != self._tex_h:
            self.resize_texture(rgba_gpu.shape[1], rgba_gpu.shape[0])

        # Ensure all prior GPU work is complete before touching the PBO
        _cudart.cudaDeviceSynchronize()

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
        """Clear the entire window to black, then set the letterboxed viewport."""
        GL.glViewport(0, 0, self._win_w, self._win_h)
        GL.glClearColor(0.0, 0.0, 0.0, 1.0)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT)
        GL.glViewport(self._vp_x, self._vp_y, self._vp_w, self._vp_h)

    def set_compare_mode(self, enabled: bool):
        """Enable or disable split-screen A/B comparison."""
        self._compare_mode = enabled

    def upload_original(self, rgba_gpu):
        """Upload the original (pre-VSR) RGBA tensor to the second GL texture.

        Expects the texture to already be at the correct size (upload_texture
        is called first to handle any resize).  Silently skips if dimensions
        don't match.
        """
        if rgba_gpu.shape[1] != self._tex_w or rgba_gpu.shape[0] != self._tex_h:
            return  # texture not sized yet — skip this frame
        _cudart.cudaDeviceSynchronize()
        dev_ptr, size = cuda_gl_map(self._cu_resource_orig)
        assert size >= self._pbo_size
        cuda_memcpy_dtod(dev_ptr, rgba_gpu.data_ptr(), self._pbo_size)
        cuda_gl_unmap(self._cu_resource_orig)

        GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex_orig)
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, self._pbo_orig)
        GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0,
                           self._tex_w, self._tex_h,
                           GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, ctypes.c_void_p(0))
        GL.glBindBuffer(GL.GL_PIXEL_UNPACK_BUFFER, 0)

    def draw_quad(self):
        """Draw the fullscreen textured quad."""
        if self._compare_mode:
            GL.glUseProgram(self._program_compare)
            GL.glBindVertexArray(self._vao)
            GL.glActiveTexture(GL.GL_TEXTURE0)
            GL.glBindTexture(GL.GL_TEXTURE_2D, self._tex_orig)
            GL.glUniform1i(self._uloc_orig, 0)
            GL.glActiveTexture(GL.GL_TEXTURE1)
            GL.glBindTexture(GL.GL_TEXTURE_2D, self._texture)
            GL.glUniform1i(self._uloc_vsr, 1)
            GL.glDrawArrays(GL.GL_TRIANGLES, 0, 6)
            GL.glActiveTexture(GL.GL_TEXTURE0)
        else:
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
        cuda_gl_unregister(self._cu_resource_orig)
        GL.glDeleteTextures(2, [self._texture, self._tex_orig])
        GL.glDeleteBuffers(2, [self._pbo, self._pbo_orig])
        GL.glDeleteBuffers(1, [self._vbo])
        GL.glDeleteVertexArrays(1, [self._vao])
        GL.glDeleteProgram(self._program)
        GL.glDeleteProgram(self._program_compare)
        glfw.destroy_window(self._window)
        glfw.terminate()
