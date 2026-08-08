#!/usr/bin/env python3
"""RIFE TensorRT 插值输出完整视频（yuv444p/bt709/tv，420 会有色块）。

用法: <vfi>/bin/python run_rife_trt_video.py <video> <W> <H> <nframes> <engine> lite <out.mp4>
输出: 60fps 流 [f0, mid0, f1, mid1, ...]（lite 引擎含 pad/grid 逻辑）。
"""
import subprocess, numpy as np, sys, time, os
import tensorrt as trt
import torch

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3]); N = int(sys.argv[4])
ENG = sys.argv[5]; OUT = sys.argv[7] if len(sys.argv) > 7 else sys.argv[6]
FR = W * H * 3

logger = trt.Logger(trt.Logger.WARNING)
with open(ENG, "rb") as f:
    engine = trt.Runtime(logger).deserialize_cuda_engine(f.read())
ctx = engine.create_execution_context()
in_name = engine.get_tensor_name(0)
out_name = engine.get_tensor_name(1)

PH, PW = int(engine.get_tensor_shape(in_name)[2]), int(engine.get_tensor_shape(in_name)[3])
DT = torch.float16 if engine.get_tensor_dtype(in_name) == trt.DataType.HALF else torch.float32
x = np.linspace(0, PW - 1, PW); y = np.linspace(0, PH - 1, PH)
gx, gy = np.meshgrid(x, y)
GH = (2 * gx / (PW - 1) - 1).astype(np.float32)
GV = (2 * gy / (PH - 1) - 1).astype(np.float32)
MH = np.full((PH, PW), 2 / (PW - 1), np.float32)
MW = np.full((PH, PW), 2 / (PH - 1), np.float32)
print(f"pad {W}x{H} -> {PW}x{PH}, out: {OUT}", flush=True)

src = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-frames:v", str(N),
     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
enc = subprocess.Popen(
    ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
     "-s", f"{W}x{H}", "-r", "60", "-i", "-",
     "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv444p",
     "-colorspace", "bt709", "-color_range", "tv", OUT], stdin=subprocess.PIPE)

def read_exact(n):
    buf = bytearray()
    while len(buf) < n:
        c = src.stdout.read(n - len(buf))
        if not c:
            return None
        buf += c
    return bytes(buf)

xin = torch.zeros((1, 11, PH, PW), dtype=DT, device="cuda")
xout = torch.zeros((1, 3, PH, PW), dtype=DT, device="cuda")
ctx.set_tensor_address(in_name, xin.data_ptr())
ctx.set_tensor_address(out_name, xout.data_ptr())
ctx.execute_async_v3(torch.cuda.current_stream().cuda_stream)
torch.cuda.synchronize()

t0 = time.time()
prev = None
n = 0
while True:
    b = read_exact(FR)
    if b is None:
        break
    cur = np.frombuffer(b, np.uint8).reshape(H, W, 3)
    if prev is None:
        prev = cur
        enc.stdin.write(prev.tobytes())   # 首帧直出
        continue
    p0 = np.zeros((PH, PW, 3), np.uint8); p0[:H, :W] = prev
    p1 = np.zeros((PH, PW, 3), np.uint8); p1[:H, :W] = cur
    x0 = torch.from_numpy(p0.astype(np.float32) / 255.0).to(DT).permute(2, 0, 1)[None].contiguous().cuda()
    x1 = torch.from_numpy(p1.astype(np.float32) / 255.0).to(DT).permute(2, 0, 1)[None].contiguous().cuda()
    t = torch.full((1, 1, PH, PW), 0.5, dtype=DT, device="cuda")
    xin.copy_(torch.cat([x0, x1, t,
                         torch.from_numpy(GH).to(DT).cuda()[None, None],
                         torch.from_numpy(GV).to(DT).cuda()[None, None],
                         torch.from_numpy(MH).to(DT).cuda()[None, None],
                         torch.from_numpy(MW).to(DT).cuda()[None, None]], dim=1))
    ctx.execute_async_v3(torch.cuda.current_stream().cuda_stream)
    torch.cuda.synchronize()
    m = (xout[0, :, :H, :W].permute(1, 2, 0).float().cpu().numpy() * 255.0)
    enc.stdin.write(np.clip(m, 0, 255).astype(np.uint8).tobytes())
    enc.stdin.write(cur.tobytes())
    n += 1
    if n % 200 == 0:
        print(f"[{time.time()-t0:.0f}s] {n} pairs", flush=True)
    prev = cur
src.kill()
enc.stdin.close()
enc.wait()
print(f"done: {OUT} ({n} pairs, {time.time()-t0:.0f}s)", flush=True)
