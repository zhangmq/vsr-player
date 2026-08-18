---
name: fruc-research-conclusion
description: OFA FRUC 调研结论（已修正 2026-08-08）：官方 FRUC 库不可用 ≠ NVOFA 引擎不可用——正确路线 = 引擎 + 自写后处理，已走通

# OFA FRUC 插帧调研结论（**2026-08-09 最终定案：RIFE lite 为主引擎**）

## 最终决策（2026-08-09，用户确认）

- **NVOFA 硬件光流路线效果不理想**：插帧 repeat 率太高——用户评价"效果不算理想"，**不再是主路线**。⚠️ 勿引用旧测量值（v4 9.5% / v5 15.5% / heya 13%）当代表值：那些是特定版本/安静段局部统计
- **全片实测 repeat 率（2026-08-09 重跑，heya_720p 全片 13439 对，官方批模式 sm_120 内核）**：REPEAT **90.8%**（复制前帧 11890 / 复制后帧 309），有运动却 repeat **11224 对（83.5%）**，有效插值仅 9.2%，**有运动对中插值成功率 9.7%**（stats_repeat.py TH=0.8 口径）。产物 /tmp/goose_of_out/heya_full_recheck.mp4
- **RIFE lite 同口径对比（2026-08-09，heya 全片 13439 对，TRT FP16 768 引擎）**：REPEAT **10.3%**（复制前帧 346 / 复制后帧 1041），有运动却 repeat **458 对（3.4%）**，有效插值 89.7%，**有运动对中插值成功率 96.3%**——成功率比 NVOFA 高近 10 倍，主引擎决策坐实。有效对居中比 1.317（mid 轻微偏后帧侧，模型特性非缺陷）。产物 /tmp/goose_of_out/heya_rife_lite_full.mp4
- **RIFE / rife lite 插帧成功率远高于 NVOFA**——RIFE lite 定为主插帧引擎（详见 [[rife-model-selection]]）
- 此前记录的"用户确认视频完美"（2026-08-08）是当时编码污染修正后的对比评价；**最终质量评价以本条为准**

## OFA FRUC 调研结论（**2026-08-08 重大修正**）

## ⚠️ 原结论错误（2026-08-06）：把"官方库不可用"扩大成了"整个 NVOFA 路线不可用"

- 原文："NVIDIA OFA FRUC 在这台机器上不可用……根因：2022 lib 与 Blackwell 不兼容"——**表述错误**：
  - **正确的部分**：`libNvOFFRUC.so`（官方闭源 FRUC 库，sm_75/sm_80 cubin 无 PTX）在 Blackwell **不可用**（2026-08-08 黑盒实测：Create 成功、Process 第一次调用 SIGSEGV）
  - **错误的部分**：**NVOFA 硬件引擎本身完全可用**（RTX 5060 Ti 实测 0.63ms/对；官方 AppOFCuda demo 720p SLOW 175fps）——"库不可用"≠"路线不可用"
- **代价**：用户被引导去折腾 RIFE（模型调研/ONNX/TRT 集成），绕了最远的路；最终正确路线（引擎 + 自写后处理）被用户逼着才走通
- **教训**：下"不可用"结论前必须区分**硬件能力**与**特定软件组件**；引用官方能力要实测引擎本身

## 正确结论（2026-08-08，用户确认"视频完美"）

**NVOFA 硬件插帧路线在 Blackwell 完全可行**，唯一不可用的是官方闭源 FRUC 库（它只做"flow→插值帧"的后处理，该部分自写即可）：

```
NVOFA 引擎（官方 API，批模式 16+15 buffer，BOTH 方向，SLOW，grid4）
  → flow（fwd+bwd，stride 布局，官方 DownloadData 下载）
  → 自写 FRUC 后处理（densify edge-aware → smooth_y/mag → occ(fwd-bwd) →
    warp 0.5 → blend，occ 回退 warp0）
  → 插值帧 → yuv444p/bt709/tv 编码（420 会有色块！）
```

- 实现：`tests/fruc/fruc_of_sm120.cu` + `run_sm120.py`（线程化防管道死锁）+ `run_sm120_png.py`
- 验证：heya 720p 1 分钟 60fps 慢放 + goose_480p 全片（9158 对）——用户确认完美
- 性能：302 帧 3s（720p SLOW+BOTH）；1 分钟视频 18s
- 关键坑：yuv444p（420 色块）、densify 网格 ceil(W/G)（854 非整除）、occ 回退目标 warp0、管道死锁（队列>批大小）

## RIFE 路线状态（2026-08-09 起为主路线）

- [[rife-model-selection]]：社区共识 4.25，整帧推理 1080p 22ms 实时可行；**rife lite 已定为主插帧引擎**（成功率高于 NVOFA，且可实时）
- 2026-08-08 时 NVOFA 曾为主（当时 RIFE 被误判色块=引擎缺陷，实为后处理阶段问题）——路线选择以最新决策为准

## 关联

- [[hardware-fruc-status]]：硬件光流 FRUC 完整状态（官方模式验证、stride 布局、编码教训）
