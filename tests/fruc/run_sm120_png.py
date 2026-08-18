#!/usr/bin/env python3
"""输出插值帧序列 PNG（mid 帧，按序 mid_%04d.png）。线程化防管道死锁。

用法: run_sm120_png.py <video> <W> <H> <nframes> <outdir>
"""
import subprocess, numpy as np, sys, os, threading, time

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3])
N = int(sys.argv[4]); OUTDIR = sys.argv[5]
FR = W * H * 3
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "build", "tests", "fruc", "fruc_of_sm120")
C12 = os.path.join(ROOT, "third_party", "cuda12", "cuda_cudart-linux-x86_64-12.9.79-archive", "lib")
env = dict(os.environ, LD_LIBRARY_PATH=C12)
os.makedirs(OUTDIR, exist_ok=True)

src = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-frames:v", str(N),
     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
fruc = subprocess.Popen([BIN, str(W), str(H)],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env)
png = subprocess.Popen(
    ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
     "-s", f"{W}x{H}", "-i", "-",
     "-f", "image2", f"{OUTDIR}/mid_%04d.png"],
    stdin=subprocess.PIPE)
t0 = time.time()

def feeder():
    buf4 = np.zeros((H, W, 4), np.uint8)
    n = 0
    while True:
        b = src.stdout.read(FR)
        if not b or len(b) != FR:
            break
        f3 = np.frombuffer(b, np.uint8).reshape(H, W, 3)
        buf4[..., 0] = f3[..., 0]; buf4[..., 1] = f3[..., 1]
        buf4[..., 2] = f3[..., 2]; buf4[..., 3] = 255
        fruc.stdin.write(buf4.tobytes())
        n += 1
        if n % 100 == 0:
            print(f"[{time.time()-t0:.0f}s] fed {n}", flush=True)
    fruc.stdin.close()
    print(f"[{time.time()-t0:.0f}s] feeder done ({n} frames)", flush=True)

def png_writer():
    n = 0
    while True:
        mid = fruc.stdout.read(FR)
        if not mid or len(mid) != FR:
            break
        png.stdin.write(mid)
        n += 1
        if n % 100 == 0:
            print(f"[{time.time()-t0:.0f}s] png {n}", flush=True)
    png.stdin.close()
    print(f"[{time.time()-t0:.0f}s] png writer done ({n} mids)", flush=True)

tf = threading.Thread(target=feeder, daemon=True)
tp = threading.Thread(target=png_writer, daemon=True)
tf.start(); tp.start()
tp.join()
fruc.wait(timeout=120)
src.kill()
png.wait()
err = fruc.stderr.read().decode()
print("fruc rc =", fruc.returncode)
print(err[-200:])
print(f"done: {OUTDIR}")
