"""Async VSR pipeline — GPU format conversion and VSR effect with CUDA streams."""

import torch
from nvvfx import VideoSuperRes


def frame_to_gpu(bgr_uint8):
    """OpenCV BGR uint8 (H,W,3) -> GPU RGB float32 (3,H,W) in [0,1]. Async H2D."""
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA-capable GPU with NVIDIA driver required")
    tensor = torch.from_numpy(bgr_uint8).cuda(non_blocking=True)
    tensor = tensor[..., [2, 1, 0]]          # BGR -> RGB
    tensor = tensor.float().mul_(1.0 / 255.0)
    tensor = tensor.permute(2, 0, 1)         # HWC -> CHW
    return tensor.contiguous()


def gpu_to_texture(rgb_float32):
    """GPU float32 RGB (3,H,W) [0,1] -> GPU uint8 RGBA (H,W,4) for GL texture."""
    tensor = rgb_float32.permute(1, 2, 0)    # CHW -> HWC
    tensor = tensor.mul(255.0).clamp(0, 255).to(torch.uint8)
    out_h, out_w = tensor.shape[:2]
    rgba = torch.zeros(out_h, out_w, 4, device=tensor.device, dtype=torch.uint8)
    rgba[..., :3] = tensor                    # RGB -> RGBA (alpha=255)
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

    def reconfigure(self, out_w: int, out_h: int, quality=None):
        """Replace the VSR instance with one configured to new params.

        A fresh ``VideoSuperRes`` is created because calling ``load()``
        after ``run()`` on the same instance produces an NvVFX invalid-
        parameter error (code -7).
        """
        if quality is not None:
            self.quality = quality
        self.out_w, self.out_h = out_w, out_h
        self.stream = torch.cuda.Stream()
        self.vsr = VideoSuperRes(quality=self.quality)
        self.vsr.output_width = out_w
        self.vsr.output_height = out_h
        self.vsr.load()

    def process_frame(self, bgr_uint8):
        """Full per-frame pipeline: BGR numpy -> VSR -> RGBA GPU tensor.

        All GPU work (H2D, VSR, format conversion) runs on self.stream
        to eliminate stream-level data hazards.
        """
        with torch.cuda.stream(self.stream):
            gpu_in = frame_to_gpu(bgr_uint8)
            result = self.vsr.run(gpu_in, non_blocking=True,
                                  stream_ptr=self.stream.cuda_stream)
            self.stream.synchronize()
            gpu_out = torch.from_dlpack(result.image).clone()
            rgba = gpu_to_texture(gpu_out)
            return rgba
