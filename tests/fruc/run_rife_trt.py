#!/usr/bin/env python3
"""RIFE 4.25 TensorRT 引擎推理（性能 + 质量评估）。

引擎: build/tests/fruc/rife_v4.25_dyn.engine（ModelOpt AutoCast FP16 + TRT 11）
CUDA 内存由 torch 分配（data_ptr 绑定 TRT I/O）。
统计口径与 run_rife.py / fruc_of_sm120 dev_hist 一致。

用法: <vfi>/bin/python run_rife_trt.py <video> <W> <H> <nframes>
"""
import subprocess, numpy as np, sys, time, os
import tensorrt as trt
import torch

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3]); N = int(sys.argv[4])
FR = W * H * 3
ENG = "/home/zmq/projects/vsr-player/build/tests/fruc/rife_v4.25_dyn.engine"

logger = trt.Logger(trt.Logger.WARNING)
with open(ENG, "rb") as f:
    engine = trt.Runtime(logger).deserialize_cuda_engine(f.read())
ctx = engine.create_execution_context()
in_name = engine.get_tensor_name(0)
out_name = engine.get_tensor_name(1)
print("engine io:", in_name, engine.get_tensor_dtype(in_name), "->", out_name)

src = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-frames:v", str(N),
     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)

def read_exact(n):
    buf = bytearray()
    while len(buf) < n:
        c = src.stdout.read(n - len(buf))
        if not c:
            return None
        buf += c
    return bytes(buf)

dev_hist = [0] * 9
sp_ = sn_ = ss_ = 0.0
prev = None
n = 0
t_infer = 0.0
# 预热（任意小 shape 触发 kernels 编译）
t0 = time.time()
ctx.set_input_shape(in_name, (1, 7, H, W))
xin = torch.zeros((1, 7, H, W), dtype=torch.float16, device="cuda")
xout = torch.zeros((1, 3, H, W), dtype=torch.float16, device="cuda")
ctx.set_tensor_address(in_name, xin.data_ptr())
ctx.set_tensor_address(out_name, xout.data_ptr())
ctx.execute_async_v3(torch.cuda.current_stream().cuda_stream)
torch.cuda.synchronize()
print(f"warmup done ({time.time()-t0:.2f}s)")
warm = time.time()

while True:
    b = read_exact(FR)
    if b is None:
        break
    cur = np.frombuffer(b, np.uint8).reshape(H, W, 3)
    if prev is None:
        prev = cur
        continue
    x0 = torch.from_numpy(prev.astype(np.float16) / 255.0).permute(2, 0, 1)[None].contiguous().cuda()
    x1 = torch.from_numpy(cur.astype(np.float16) / 255.0).permute(2, 0, 1)[None].contiguous().cuda()
    t = torch.full((1, 1, H, W), 0.5, dtype=torch.float16, device="cuda")
    ctx.set_input_shape(in_name, (1, 7, H, W))
    xin.copy_(torch.cat([x0, x1, t], dim=1))
    ts = time.time()
    ctx.execute_async_v3(torch.cuda.current_stream().cuda_stream)
    torch.cuda.synchronize()
    t_infer += time.time() - ts
    m = (xout[0].permute(1, 2, 0).float().cpu().numpy() * 255.0).astype(np.float32)
    p = prev.astype(np.float32); c = cur.astype(np.float32)
    dp = float(np.mean(np.abs(m - p))); dn = float(np.mean(np.abs(m - c)))
    ds = float(np.mean(np.abs(c - p)))
    dev = dp + dn - ds
    dbk = 0 if ds <= 0.5 else (1 if dev < 1 else 2 if dev < 2 else 3 if dev < 3 else
            4 if dev < 4 else 5 if dev < 6 else 6 if dev < 8 else 7 if dev < 12 else 8)
    dev_hist[dbk] += 1
    sp_ += dp; sn_ += dn; ss_ += ds
    n += 1
    if n % 100 == 0:
        print(f"[{time.time()-warm:.0f}s] {n} pairs", flush=True)
    prev = cur
src.kill()

per = t_infer / n * 1000
print(f"\n== RIFE TRT FP16: {os.path.basename(V)} {W}x{H} {n} 对")
print(f"推理 {per:.1f} ms/对 ({n/t_infer:.1f} 对/s); 总 {time.time()-warm:.1f}s")
print(f"均值: dp={sp_/n:.2f} dn={sn_/n:.2f} ds={ss_/n:.2f} dev={(sp_+sn_-ss_)/n:.2f}")
print("dev 分布 (static,<1,<2,<3,<4,<6,<8,<12,>=12):", " ".join(map(str, dev_hist)))
hi = sum(dev_hist[4:]); hi2 = sum(dev_hist[5:])
print(f"dev>=4 (伪影风险): {hi} ({hi/n*100:.1f}%)  dev>=6 (明显): {hi2} ({hi2/n*100:.1f}%)")
