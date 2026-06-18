"""NV12-to-RGB GPU conversion — zero-copy from NVDEC output to VSR input.

Two code paths:
1. **HW path** (is_hw=True): Frame is GPU NV12. D2D copy plane data into
   PyTorch tensors, then pure PyTorch ops for UV upsampling + YUV→RGB.
   All on CUDA — zero PCIe transfer.
2. **SW path** (is_hw=False): Frame is on CPU. Use PyAV's to_ndarray(rgb24)
   then H2D copy + normalize.
"""

import ctypes
from ctypes import c_void_p, c_size_t, c_int
import glob as _glob

import av
import torch


# ── CUDA runtime API (libcudart.so) ──────────────────────────────────

def _find_libcudart():
    """Locate libcudart.so — same strategy as renderer.py."""
    candidates = []
    for pattern in [
        "/home/zmq/miniforge3/envs/vsr-player/lib/python*/site-packages/nvidia/cu*/lib/libcudart.so*",
        "/opt/cuda/lib64/libcudart.so*",
        "/opt/cuda/targets/*/lib/libcudart.so*",
        "/usr/lib/libcudart.so*",
        "/usr/local/cuda/lib64/libcudart.so*",
    ]:
        candidates.extend(sorted(_glob.glob(pattern)))
    unversioned = [p for p in candidates if p.endswith('.so')]
    if unversioned:
        return unversioned[0]
    if candidates:
        return candidates[-1]
    raise RuntimeError("libcudart.so not found")

_cudart = ctypes.CDLL(_find_libcudart())
_cudart.cudaMemcpy.argtypes = [c_void_p, c_void_p, c_size_t, c_int]
_cudart.cudaMemcpy.restype = c_int
_CUDA_MEMCPY_D2D = 3


# ── D2D copy helper ──────────────────────────────────────────────────

def _d2d_copy_plane(frame: "av.VideoFrame", plane_idx: int
                    ) -> torch.Tensor:
    """Copy one plane from a GPU NV12 frame into a new PyTorch tensor."""
    plane = frame.planes[plane_idx]
    size = plane.buffer_size
    if plane_idx == 0:
        h, w = frame.height, frame.width
    else:
        h, w = frame.height // 2, frame.width  # UV interleaved row width = W

    tensor = torch.empty(h, w, dtype=torch.uint8, device="cuda")
    ret = _cudart.cudaMemcpy(
        c_void_p(tensor.data_ptr()),
        c_void_p(plane.buffer_ptr),
        c_size_t(size),
        _CUDA_MEMCPY_D2D,
    )
    if ret != 0:
        raise RuntimeError(f"cudaMemcpy D2D plane {plane_idx} failed: {ret}")
    return tensor


# ── Hardware path: NV12 on GPU → float32 RGB (3,H,W) [0,1] ──────────

def _nv12_gpu_to_rgb(y: torch.Tensor, uv: torch.Tensor) -> torch.Tensor:
    """(H,W) uint8 Y + (H/2,W) uint8 interleaved UV → (3,H,W) float32 [0,1].

    Uses nearest-neighbour UV upsampling and BT.601 full-range coefficients.
    All operations run on CUDA.
    """
    H, W = y.shape

    # Extract interleaved U and V:
    #   UV plane layout: U0 V0 U1 V1 ... per row
    #   reshape to (H/2, W/2, 2) where [..., 0] = U, [..., 1] = V
    uv_r = uv.view(H // 2, W // 2, 2)          # (H/2, W/2, 2)
    u = uv_r[:, :, 0].float()                   # (H/2, W/2)
    v = uv_r[:, :, 1].float()                   # (H/2, W/2)

    y_f = y.float()                              # (H, W)

    # Nearest-neighbour upsample chroma to luma resolution
    u = u.repeat_interleave(2, dim=0).repeat_interleave(2, dim=1)  # (H, W)
    v = v.repeat_interleave(2, dim=0).repeat_interleave(2, dim=1)  # (H, W)

    # BT.601 full-range YUV → RGB
    #   R = Y                + 1.402   * (V - 128)
    #   G = Y - 0.344 * (U - 128) - 0.714 * (V - 128)
    #   B = Y + 1.773 * (U - 128)
    u_off = u - 128.0
    v_off = v - 128.0

    r = y_f + 1.402 * v_off
    g = y_f - 0.34414 * u_off - 0.71414 * v_off
    b = y_f + 1.772 * u_off

    # Stack as CHW, clamp, normalize
    rgb = torch.stack([r, g, b], dim=0)          # (3, H, W)
    rgb = rgb.clamp(0.0, 255.0).div_(255.0)
    return rgb.contiguous()


# ── Software path: CPU frame → GPU float32 RGB (3,H,W) [0,1] ────────

def _sw_frame_to_rgb(frame: "av.VideoFrame") -> torch.Tensor:
    """CPU frame (any pixel format) → GPU float32 RGB (3,H,W) via H2D."""
    rgb_cpu = frame.to_ndarray(format="rgb24")    # (H, W, 3) uint8
    rgb_gpu = torch.from_numpy(rgb_cpu).cuda(non_blocking=True)
    rgb_gpu = rgb_gpu.float().mul_(1.0 / 255.0)
    rgb_gpu = rgb_gpu.permute(2, 0, 1).contiguous()  # HWC → CHW
    return rgb_gpu


# ── Public API ───────────────────────────────────────────────────────

def convert_frame_to_rgb(frame: "av.VideoFrame",
                         is_hw: bool = False) -> torch.Tensor:
    """Convert a PyAV VideoFrame to GPU float32 RGB (3,H,W) in [0,1].

    Args:
        frame: PyAV VideoFrame from :class:`Decoder`.
        is_hw: ``True`` if the frame is GPU NV12 from NVDEC.

    Returns:
        ``torch.Tensor`` of shape ``(3, H, W)``, dtype ``float32``,
        device ``cuda``, values in ``[0.0, 1.0]`` — ready for VSR.
    """
    if is_hw and frame.format.name == "nv12":
        y = _d2d_copy_plane(frame, 0)
        uv = _d2d_copy_plane(frame, 1)
        return _nv12_gpu_to_rgb(y, uv)
    else:
        return _sw_frame_to_rgb(frame)
