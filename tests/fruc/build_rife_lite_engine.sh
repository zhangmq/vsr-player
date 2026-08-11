#!/bin/bash
# 用系统 TensorRT (trtexec) 构建 RIFE 4.25 lite 动态 shape FP16 引擎。
#
# 与 full 引擎同构（2026-08-12 统一）：
#   - 动态 shape（min=128x128, opt=1152x1920, max=2176x3840）：一个引擎
#     覆盖所有 ≤4K 尺寸（官方 7ch 模型内部处理任意分辨率，实测非 128
#     倍数 854x480 输出 999dB vs ORT——原 11ch vs-mlrt 外部 grid 版有
#     128 对齐要求，已退役）
#   - ONNX: rife_lite_fp16_all.onnx（ModelOpt 全 FP16，IO FP16 匹配
#     rife_proc 的 FP16 内核；autocast 变体 IO FP32 会数据错位）
#   - 输入 7 通道 [1,7,PH,PW] = img0(RGB) + img1(RGB) + t(1)（grid 内部）
#   - 播放器按视频原尺寸 set_shape 推理（无 pad）；超 max 降级 passthrough
#
# 用法: bash build_rife_lite_engine.sh [out.engine]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=${1:-"$HERE/../../build/tests/fruc/rife_lite_fp16.engine"}
ONNX="$HERE/../../build/tests/fruc/rife_lite_fp16_all.onnx"

echo "=== building lite dynamic engine (min 128x128 / opt 1152x1920 / max 2176x3840) ==="

trtexec --onnx="$ONNX" \
    "--minShapes=input:1x7x128x128" \
    "--optShapes=input:1x7x1152x1920" \
    "--maxShapes=input:1x7x2176x3840" \
    --saveEngine="$OUT" 2>&1 | grep -E "\[E\]|FAILED|Total Host Walltime" || true

if [ -f "$OUT" ]; then
    echo "=== engine saved: $OUT ($(stat -c%s "$OUT") bytes) ==="
else
    echo "=== BUILD FAILED ===" >&2
    exit 1
fi
