#!/usr/bin/env python3
"""RIFE 4.25 插帧对照实验：与 NVOFA 引擎同统计口径（dp/dn/ds/dev）。

对照逻辑：NVOFA 对不可靠内容回退 prev（repeat）；RIFE 无条件输出插值帧，
其"失败"表现为 dev（线段偏移）高 = 鬼影/扭曲。dev 分桶与
fruc_of_sm120.cu 的 dev_hist 一致（<1,<2,<3,<4,<6,<8,<12,>=12）。

用法: LD_LIBRARY_PATH=<nvidia cu12 libs> <vfi>/bin/python run_rife.py <video> <W> <H> <nframes>
  库路径: $(python3 -c 'import site;print(site.getsitepackages()[0])')/nvidia/{cublas,cufft,cudnn,curand,cusolver,cusparse,cuda_runtime,nvjitlink}/lib
依赖: miniforge3 envs/vfi（onnxruntime-gpu 1.24 + nvidia cu12 pip 库）
"""
import subprocess, numpy as np, sys, time, os, re

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3]); N = int(sys.argv[4])
FR = W * H * 3
ONNX = "/home/zmq/projects/vsr-player/third_party/rife/rife_v4.25.onnx"

import onnxruntime as ort
s = ort.InferenceSession(ONNX, providers=["CUDAExecutionProvider", "CPUExecutionProvider"])
assert s.get_providers()[0] == "CUDAExecutionProvider", "CUDA EP 不可用"

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
        continue
    # RIFE 输入: [1,7,H,W] = img0(3) + img1(3) + timestep(1)
    x0 = prev.astype(np.float32) / 255.0
    x1 = cur.astype(np.float32) / 255.0
    x0 = x0.transpose(2, 0, 1)[None]
    x1 = x1.transpose(2, 0, 1)[None]
    t = np.full((1, 1, H, W), 0.5, np.float32)
    mid = s.run(None, {"input": np.concatenate([x0, x1, t], axis=1)})[0][0]
    m = (np.clip(mid, 0, 1).transpose(1, 2, 0) * 255).astype(np.float32)
    p = prev.astype(np.float32); c = cur.astype(np.float32)
    dp = float(np.mean(np.abs(m - p))); dn = float(np.mean(np.abs(m - c)))
    ds = float(np.mean(np.abs(c - p)))
    dev = dp + dn - ds
    dbk = 0 if ds <= 0.5 else (1 if dev < 1 else 2 if dev < 2 else 3 if dev < 3 else
            4 if dev < 4 else 5 if dev < 6 else 6 if dev < 8 else 7 if dev < 12 else 8)
    dev_hist[dbk] += 1
    sp_ += dp; sn_ += dn; ss_ += ds
    n += 1
    if n % 50 == 0:
        print(f"[{time.time()-t0:.0f}s] {n} pairs", flush=True)
    prev = cur
src.kill()

print(f"\n== RIFE 4.25: {os.path.basename(V)} {W}x{H} {n} 对 ({time.time()-t0:.1f}s, "
      f"{n/(time.time()-t0):.1f} 对/s)")
print(f"均值: dp={sp_/n:.2f} dn={sn_/n:.2f} ds={ss_/n:.2f} dev={(sp_+sn_-ss_)/n:.2f}")
print("dev 分布 (static,<1,<2,<3,<4,<6,<8,<12,>=12):", " ".join(map(str, dev_hist)))
# dev 高（伪影/鬼影）帧比例 —— 对照 NVOFA 的 repeat%
hi = sum(dev_hist[4:])   # dev>=4
print(f"dev>=4 (伪影风险) 帧: {hi} ({hi/n*100:.1f}%)")
hi2 = sum(dev_hist[5:])  # dev>=6
print(f"dev>=6 (明显伪影) 帧: {hi2} ({hi2/n*100:.1f}%)")
