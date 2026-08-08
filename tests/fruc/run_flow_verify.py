#!/usr/bin/env python3
"""流式验证：ffmpeg -> ABGR8 -> fruc_of_flow（官方批模式）-> flow 统计。

同时计算源帧间 mean abs diff，与 flow 突变（diff_prev 大）对照——
flow 突变若伴随帧间差大 = 内容变化（场景切换/大运动，引擎行为正常）；
无帧间差却 flow 突变 = 引擎状态异常（退化信号）。

用法: run_flow_verify.py <video> <W> <H> [nframes]
"""
import subprocess, numpy as np, sys, re

V = sys.argv[1]; W = int(sys.argv[2]); H = int(sys.argv[3])
nframes = int(sys.argv[4]) if len(sys.argv) > 4 else 0
FR = W * H * 3

ffmpeg_args = ["ffmpeg", "-v", "error", "-i", V]
if nframes:
    ffmpeg_args += ["-frames:v", str(nframes)]   # -i 之后 = input 选项
ffmpeg_args += ["-f", "rawvideo", "-pix_fmt", "rgb24", "-"]

src = subprocess.Popen(ffmpeg_args, stdout=subprocess.PIPE)
fruc = subprocess.Popen(["./fruc_of_flow", str(W), str(H)],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE)

buf4 = np.zeros((H, W, 4), np.uint8)
prev = None
n = 0
frame_diffs = []      # 每对 (pair, src mean abs diff)

while True:
    b = src.stdout.read(FR)
    if not b or len(b) != FR:
        break
    f3 = np.frombuffer(b, np.uint8).reshape(H, W, 3)
    buf4[..., 0] = f3[..., 0]
    buf4[..., 1] = f3[..., 1]
    buf4[..., 2] = f3[..., 2]
    buf4[..., 3] = 255
    if prev is not None:
        frame_diffs.append((n - 1, float(np.abs(f3.astype(np.float32) - prev).mean())))
    fruc.stdin.write(buf4.tobytes())
    prev = f3.astype(np.float32)
    n += 1

fruc.stdin.close()
src.kill()
log = fruc.stderr.read().decode()
fruc.wait()

# ---- 分析 ----
flowdiff = {}
for l in log.splitlines():
    m = re.match(r"pair (\d+) .*diff_prev=([\d.-]+)", l)
    if m:
        flowdiff[int(m.group(1))] = float(m.group(2))

mut = [p for p, d in flowdiff.items() if p > 0 and d > 2.0]
fd = dict(frame_diffs)
print(f"frames={n} pairs={len(flowdiff)}")
print("flow 突变对 (diff_prev>2):", mut[:40])
print("\npair  flow_diff  src_frame_diff  判定")
for p in mut[:25]:
    s = fd.get(p, -1)
    verdict = "内容突变(帧差大)" if s > 8 else ("内容中等" if s > 3 else "??? 无帧差,疑似引擎")
    print(f"{p:6d}  {flowdiff[p]:9.2f}  {s:13.2f}  {verdict}")
print("\n" + log.splitlines()[-1])
