#!/usr/bin/env python3
"""流式管线（线程化，防管道死锁）：ffmpeg 提取 -> ABGR8 -> fruc_of_sm120
-> 交错 [f0,mid0,f1,mid1..] -> ffmpeg 60fps 编码。

死锁背景：720p ABGR 一帧 3.7MB >> 管道 64KB；批模式下引擎处理 16 帧期间
不读 stdin，单线程逐帧喂+收会互相等死。线程分离喂/收，队列背压。

用法: run_sm120.py <video> <W> <H> <nframes> <out.mp4>
"""
import subprocess, numpy as np, sys, os, queue, threading, time

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3])
N = int(sys.argv[4]); OUT = sys.argv[5]
FR = W * H * 3
BIN = "/home/zmq/projects/vsr-player/build/tests/fruc/fruc_of_sm120"
C12 = "/home/zmq/projects/vsr-player/third_party/cuda12/cuda_cudart-linux-x86_64-12.9.79-archive/lib"
env = dict(os.environ, LD_LIBRARY_PATH=C12)

src = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-frames:v", str(N),
     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
fruc = subprocess.Popen([BIN, str(W), str(H)],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env)
enc = subprocess.Popen(
    ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
     "-s", f"{W}x{H}", "-r", "60", "-i", "-",
     "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p",
     OUT], stdin=subprocess.PIPE)

src_q = queue.Queue(maxsize=32)     # >批大小16，防死锁
mid_q = queue.Queue(maxsize=32)     # >批大小16，防死锁
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
        src_q.put(f3.tobytes())
        fruc.stdin.write(buf4.tobytes())
        n += 1
        if n % 50 == 0:
            print(f"[{time.time()-t0:.0f}s] fed {n} frames", flush=True)
    fruc.stdin.close()
    print(f"[{time.time()-t0:.0f}s] feeder done ({n} frames)", flush=True)

def mid_reader():
    while True:
        mid = fruc.stdout.read(FR)
        if not mid or len(mid) != FR:
            mid_q.put(None)
            break
        mid_q.put(mid)

def collector():
    first = src_q.get()
    enc.stdin.write(first)
    n = 0
    while True:
        try:
            mid = mid_q.get(timeout=300)
        except queue.Empty:
            print("mid timeout", flush=True); break
        if mid is None:
            break
        f = src_q.get()
        enc.stdin.write(mid)
        enc.stdin.write(f)
        n += 1
        if n % 50 == 0:
            print(f"[{time.time()-t0:.0f}s] interleaved {n} pairs", flush=True)
    enc.stdin.close()
    print(f"[{time.time()-t0:.0f}s] collector done ({n} pairs)", flush=True)

tf = threading.Thread(target=feeder, daemon=True)
tm = threading.Thread(target=mid_reader, daemon=True)
tc = threading.Thread(target=collector, daemon=True)
tf.start(); tm.start(); tc.start()
tc.join()
fruc.stdin.close()
try:
    fruc.wait(timeout=120)
except subprocess.TimeoutExpired:
    print("FRUC HUNG - killing"); fruc.kill()
err = fruc.stderr.read().decode()
print("fruc rc =", fruc.returncode)
print(err[-400:])
src.kill()
enc.wait()
print(f"done: {OUT}")
