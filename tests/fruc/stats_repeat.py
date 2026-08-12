#!/usr/bin/env python3
"""goose 480p 全片 repeat 统计 + 中间帧/前后帧差异对比。

输入: 插值输出视频（60fps，流结构 [s0, m0, s1, m1, ...]）
  s_k = 源帧（偶数位置），m_k = (s_k, s_{k+1}) 的插值帧（奇数位置）
判定（mean abs diff，0-255 尺度）:
  d_prev = |m_k - s_k|,  d_next = |m_k - s_{k+1}|,  d_src = |s_k - s_{k+1}|
  REPEAT: min(d_prev, d_next) < TH（插值帧复制了某一侧源帧）
  有效:   两侧都 >= TH；居中比 r = (d_prev + d_next) / d_src 应 ≈ 1.0
          且 d_prev/d_next 各 ≈ d_src/2（t=0.5 精确居中）
  场景切换（d_src > SC_MAE，内容跳变）: 直通原帧为正确行为（vs-mlrt
  SceneChangeNext 约定），单独归类不计入引擎失败——插值帧==前帧/后帧
  均为正确直通。

用法: stats_repeat.py <video> [W] [H]
"""
import subprocess, numpy as np, sys

V = sys.argv[1]
W, H = int(sys.argv[2]) if len(sys.argv) > 2 else 854, \
       int(sys.argv[3]) if len(sys.argv) > 3 else 480
FR = W * H * 3
TH = 0.8  # mean abs diff 阈值（0-255 尺度）——压缩噪声 <0.5，复制帧 <0.2
SC_MAE = 20.0  # 场景切换阈值——内容跳变 > 正常运动

proc = subprocess.Popen(
    ["ffmpeg", "-v", "error", "-i", V, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
    stdout=subprocess.PIPE)

s_prev = None      # s_k
m_cur = None       # m_k 缓存，等 s_{k+1} 到达后判定
n_src = n_mid = n_rep = n_eff = 0
rep_prev = rep_next = 0
n_still = 0        # 源静止对 (d_src < TH)
n_rep_motion = 0   # 有运动却 repeat（失败）
n_eff_motion = 0   # 有运动且插值有效
n_sc = 0           # 场景切换对 (d_src > SC_MAE)
n_sc_pass = 0      # 切换且直通（输出 == 任一源帧，正确行为）
sum_r = sum_r2 = 0.0
samples = []       # 每 500 对采 1 个有效样本
first_mid_prev = first_mid_next = None   # 首对对比表
pos = 0
while True:
    b = proc.stdout.read(FR)
    if not b or len(b) != FR:
        break
    f = np.frombuffer(b, np.uint8).reshape(H, W, 3).astype(np.float32)
    if pos % 2 == 0:                       # 源帧 s_k
        if s_prev is not None and m_cur is not None:   # 判定上对 (s_{k-1}, m_{k-1})
            d_prev = np.mean(np.abs(m_cur - s_prev))
            d_next = np.mean(np.abs(m_cur - f))
            d_src = np.mean(np.abs(f - s_prev))
            n_mid += 1
            if d_src < TH:
                n_still += 1
            if d_src > SC_MAE:
                # 场景切换：直通任一源帧为正确（SceneChangeNext 约定）
                n_sc += 1
                if min(d_prev, d_next) < TH:
                    n_sc_pass += 1
            elif min(d_prev, d_next) < TH:
                n_rep += 1
                if d_prev < d_next:
                    rep_prev += 1
                else:
                    rep_next += 1
                if d_src >= TH:
                    n_rep_motion += 1
            else:
                n_eff += 1
                if d_src >= TH:
                    n_eff_motion += 1
                r = (d_prev + d_next) / d_src if d_src > 0 else 1.0
                sum_r += r; sum_r2 += r * r
                if n_mid % 500 == 1:
                    samples.append((n_mid, d_prev, d_next, d_src, r))
                if first_mid_prev is None:
                    first_mid_prev, first_mid_next = d_prev, d_next
        s_prev = f
    else:                                  # mid 帧 m_k
        m_cur = f
    pos += 1
proc.wait()

total = n_mid
n_motion = total - n_still - n_sc   # 有运动且非场景切换的对
print(f"总帧对: {total}")
print(f"源静止对 (d_src<{TH}, 内容无运动): {n_still} ({n_still/total*100:.1f}%)")
print(f"场景切换对 (d_src>{SC_MAE}, 直通正确): {n_sc} ({n_sc/total*100:.1f}%)"
      f"  └─ 直通前/后帧: {n_sc_pass} ({n_sc_pass/max(1,n_sc)*100:.1f}%)"
      f", 仍插值: {n_sc-n_sc_pass}")
print(f"有运动非切换对 (d_src>={TH} 且 <= {SC_MAE}): {n_motion} ({n_motion/total*100:.1f}%)")
print()
print(f"REPEAT 合计: {n_rep} ({n_rep/total*100:.1f}%)  复制前帧 {rep_prev}, 复制后帧 {rep_next}")
print(f"  ├─ 源静止下的 repeat（正常，无运动可插）: {n_rep-n_rep_motion}")
print(f"  └─ 有运动却 repeat（引擎失败）: {n_rep_motion}")
print(f"有效插值: {n_eff} ({n_eff/total*100:.1f}%)")
print(f"  └─ 其中有运动且插值有效: {n_eff_motion} ({n_eff_motion/total*100:.1f}%)")
print(f"有运动非切换对中插值成功率: {n_eff_motion/max(1,n_motion)*100:.1f}%")
if n_eff:
    r_mean = sum_r / n_eff
    r_std = (sum_r2 / n_eff - r_mean ** 2) ** 0.5
    print(f"\n有效对居中比 r=(d_prev+d_next)/d_src: 均值 {r_mean:.3f} ± {r_std:.3f}  (1.0 = 时间精确居中)")
    if samples:
        print(f"有效对 d_prev/d_src 均值: {sum(d[1]/d[3] for d in samples)/len(samples):.3f}"
              f"  (0.5 = 与前后帧等距)")
print("\n采样对比表（每 500 对取 1 个有效样本）:")
print(f"{'对#':>7} {'d_prev':>8} {'d_next':>8} {'d_src':>8} {'居中比r':>8} {'d_prev/d_src':>11}")
for n, dp, dn, ds, r in samples:
    print(f"{n:>7} {dp:>8.2f} {dn:>8.2f} {ds:>8.2f} {r:>8.3f} {dp/ds if ds>0 else 1:>11.3f}")
if first_mid_prev is not None:
    print(f"\n首对参考: d_prev={first_mid_prev:.2f} d_next={first_mid_next:.2f}")
