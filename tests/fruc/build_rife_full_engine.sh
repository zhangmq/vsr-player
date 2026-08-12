#!/bin/bash
# 用系统 TensorRT (trtexec) 构建 RIFE 4.26 full 动态 shape FP16 引擎。
#
#   - 动态 shape（min=128x128, opt=1152x1920, max=2176x3840）：一个引擎
#     覆盖所有 ≤4K 尺寸（官方模型任意分辨率——内部处理，无对齐约束）。
#   - ONNX: rife_full_fp16_all.onnx（ModelOpt 全 FP16 变体）——必须 all
#     （IO FP16）：autocast 变体（IO FP32）与 rife_proc 的 FP16 内核不
#     匹配（数据错位 → 花屏，实测 10.9dB）。源 = AmusementClub/vs-mlrt
#     external-models rife_v4.26.7z 的 rife_v2 变体（7ch——主变体是
#     11ch external-grid，与本管线不兼容）
#   - 输入 7 通道 [1,7,PH,PW] = img0(RGB) + img1(RGB) + t(1)；grid 在
#     模型内部生成
#   - 播放器按视频原尺寸 set_shape 推理（无 pad）；超 max 降级 passthrough
#   - 4.26 vs 4.25：质量全档胜出（合成 GT PSNR +1~4.6dB，中位移优势最大）、
#     性能持平（±5% 测量噪声）——2026-08-12 实测切换
#
# 用法: bash build_rife_full_engine.sh [out.engine]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=${1:-"$HERE/../../build/tests/fruc/rife_full_fp16.engine"}
ONNX="$HERE/../../build/tests/fruc/rife_full_fp16_all.onnx"

echo "=== building full dynamic engine (min 128x128 / opt 1152x1920 / max 2176x3840) ==="

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
