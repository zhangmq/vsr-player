# TRT 引擎调研（2026-08-10 起，2026-08-12 修正）：动态引擎可行性 + TRT 降级评估

> 状态：**动态 shape 引擎已在系统 TRT 11.1 验证可用（lite/full 均实测正确）——
> 固定 shape 矩阵方案已被动态单引擎取代**。TRT 10 降级调研挂起（下载受限，
> 且动态引擎问题已解决，TRT 10 不再是必要路径）。

## 背景

- 竖屏视频（如 z_bilibili_vertical_1080x1920.mp4）在固定 shape 引擎矩阵里缺引擎
- 固定 shape 引擎矩阵：每尺寸一引擎（构建 3 秒/个，但矩阵需维护）
- 动态 shape 引擎（一个覆盖所有尺寸）是理想解——需验证正确性

## 已验证结论（2026-08-12 修正后）

### 1. 动态 shape 引擎在 TRT 11.1 完全可用（原"不可用"结论作废）

**原结论（2026-08-10）**："TRT 11.1 动态引擎非 opt shape 输出错误（致命）"
——**作废**。验证方法有缺陷：

- 使用了**随机输入**：RIFE 对噪声输入非线性放大，TRT/ORT/FP32 输出差异巨大
  （~10dB）——随机输入不是有效测试（真实帧下 37-43dB）
- "vs 固定引擎"对比对象不清（当时矩阵是 lite 11ch，动态引擎是 full 7ch——
  模型不同，MAE 高是模型差异）

**正确结论（2026-08-12 实测，系统 TRT 11.1.0.106 + 真实帧）**：

| 验证 | 方法 | 结果 |
|---|---|---|
| full 动态 @ 非 opt 854x480 | C++ 工具 + ORT 真值 | **43.5 dB** |
| lite 动态 @ 非 128 倍数 854x480 | C++ 工具 + ORT 真值 | **999 dB**（逐字节一致） |
| lite 动态 @ 128 对齐 768x1280 | C++ 工具 + ORT 真值 | **999 dB** |
| lite 动态 @ opt 1152x1920 | C++ 工具 + ORT 真值 | **999 dB** |
| full 动态 @ 多 shape + context 切换序列 | vfi 11.2 + ORT 真值 | 37-38 dB 全部正确 |

- sm_120 autotuner bug 仅在 TRT 11.2.1.2 pip 版复现；**系统 TRT 11.1 动态构建
  成功无异常**（lite/full 均实测）
- lite 的"128 对齐要求"是 **11ch 版**（vs-mlrt 外部 grid 转换）的特征；官方
  **7ch 版**（grid 模型内部）无对齐约束——854x480 非 128 倍数也逐字节正确

### 2. 花屏根因（2026-08-11）：autocast ONNX 的 IO dtype 与内核不匹配

- RIFE 播放器管线：assemble 内核**硬编码 FP16 写入**引擎输入 buffer
- `rife_autocast_fp16.onnx`（ModelOpt AutoCast，keep_io_types=True）→ 引擎
  **IO 为 FP32** → FP16 内核写 FP32 引擎输入 = 数据错位 → 花屏（10.9dB）
- `rife_full_fp16_all.onnx`（keep_io_types=False，全 FP16）→ 引擎 IO FP16 →
  匹配 → 正常（25.5dB）
- **教训：ONNX 转换变体（autocast/all）必须与内核 dtype 匹配**；构建/推理
  脚本注释记录了此坑（build_rife_full_engine.sh）

### 3. 动态单引擎方案（2026-08-12 定案）

- lite/full 各 1 个动态引擎（profile min 128x128 / opt 1152x1920 / max
  2176x3840）覆盖所有 ≤4K 尺寸含竖屏
- 播放器按视频原尺寸 set_shape 推理（无 pad、无矩阵、无对齐约束——官方
  模型内部处理任意分辨率，联网确认 megvii-research/ECCV2022-RIFE）
- 内核统一 7ch（img0+img1+t；grid 在模型内部生成）
- 超 profile max 降级 passthrough

## TRT 10 降级评估（挂起——不再是必要路径）

### 途径盘点（2026-08-11 全部实测）

| 途径 | 结果 |
|---|---|
| NVIDIA 官网 TAR（developer.nvidia.com） | 直链 403（需登录+license 签名）；用户网络国际出口 ~50KB/s，下载不可行 |
| pypi.nvidia.com wheel | S3 签名认证，全路径 404 |
| AUR | 只有 python-tensorrt 11.1.0.106-1（即系统现用），无 10.x |
| anaconda nvidia channel / conda-forge | 均无 tensorrt 包 |
| 国内镜像（清华/阿里云/中科大） | 无（清华 /nvidia/ 已下架 404） |
| docker hub / huggingface / npm | 均无官方分发 |
| NGC 容器（nvcr.io/nvidia/tensorrt） | 可达（manifest 验证）但用户否决（不可复现路径） |

### 关键事实

- **VFX 不需要 TRT**（readelf/nm 实证，见 [[vfx-no-trt10-needed]]）：VFX 全家
  对 TRT 零符号调用，cudnn 仅 cudnnGetVersion——player 进程只有 RIFE 需要 TRT
- **TRT 10.8 没有 cuda-12.9 变体**（官方下载页实测：10.8 最高 cuda-12.8；
  cuda-12.9 变体从 10.10 起）——若下载恢复，推荐 10.13.3（cuda-12.9 TAR，
  10.13 系列成熟稳定点）
- vf_rife 加载：`rife_trt.cpp` 硬编码 `dlopen("libnvinfer.so.11")`
- TRT 与构建 CUDA（/opt/cuda 13.3）无编译时交互（dlopen 运行时加载）

### 结论

动态 shape 问题已在系统 TRT 11.1 解决 → TRT 10 降级的**原始动机（动态引擎
可行性）已消除**。TRT 10 调研保留为"引擎构建器版本差异"的参考（11.1 vs
11.2 构建器行为确实不同：autotuner bug、动态输出正确性），无紧急行动项。

## 关联

- [[rife-implementation-status]]（引擎矩阵、构建脚本）
- [[vfx-no-trt10-needed]]（VFX 不需要 TRT 实证）
- [[vfx-trt-symlink-rename]]（TRT 10/11 软链改名历史）
- 引擎：`rife_{lite,full}_fp16.engine`（动态，无尺寸后缀）；构建：
  `tests/fruc/build_rife_{lite,full}_engine.sh`；ONNX 转换：
  `tests/fruc/convert_rife_onnx_fp16.py <src> <dst> autocast|all`
