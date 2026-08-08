#!/usr/bin/env python3
"""流式插值管线：ffmpeg 提取 -> ABGR8 -> fruc_of_interp（官方模式）-> 交错
[f0, mid0, f1, mid1, ...] -> ffmpeg 60fps 编码。

用法: run_interp.py <video> <W> <H> <nframes> <out.mp4>
"""
import subprocess, numpy as np, sys, os

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3])
N = int(sys.argv[4]); OUT = sys.argv[5]
FR = W * H * 3
os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
env = dict(os.environ,
           LD_LIBRARY_PATH="/home/zmq/projects/vsr-player/third_party/cuda12/"
                           "cuda_cudart-linux-x86_64-12.9.79-archive/lib")

src = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-frames:v", str(N),
     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
    stdout=subprocess.PIPE)
fruc = subprocess.Popen(
    [os.path.join(os.path.dirname(__file__), "fruc_of_interp"), str(W), str(H)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
enc = subprocess.Popen(
    ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
     "-s", f"{W}x{H}", "-r", "60", "-i", "-",
     "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p",
     OUT],
    stdin=subprocess.PIPE)

buf4 = np.zeros((H, W, 4), np.uint8)
prev = None
n = 0
while True:
    b = src.stdout.read(FR)
    if not b or len(b) != FR:
        break
    f3 = np.frombuffer(b, np.uint8).reshape(H, W, 3)
    buf4[..., 0] = f3[..., 0]; buf4[..., 1] = f3[..., 1]
    buf4[..., 2] = f3[..., 2]; buf4[..., 3] = 255
    if prev is not None:
        enc.stdin.write(prev.tobytes())                    # f_k
        mid = fruc.stdout.read(FR)                          # mid_k
        if not mid or len(mid) != FR:
            break
        enc.stdin.write(mid)
    fruc.stdin.write(buf4.tobytes())
    prev = f3
    n += 1
    if n % 100 == 0:
        print(f"frame {n}", flush=True)

enc.stdin.write(prev.tobytes())                             # 最后一帧
enc.stdin.close()
fruc.stdin.close()
fruc.wait(timeout=300)
src.kill()
enc.wait()
print(f"done: {OUT} ({n} frames)")
