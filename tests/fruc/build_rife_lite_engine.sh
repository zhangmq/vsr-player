#!/bin/bash
# 用系统 TensorRT (trtexec, 跟随 pacman 滚动更新) 构建 RIFE lite 固定 shape FP16 引擎。
#
# 背景:
#   - 必须用 rife_lite_fp16_all.onnx（全 FP16 无 cast）：实测 rife_lite_fp16.onnx
#     (AutoCast 带入口/出口 cast) 构建的引擎在第 3 对起输出全 nan——
#     TRT 11.1.0.106 和 11.2.1.2 构建器均复现（未知构建器 bug）
#   - all 变体构建的引擎 IO 为 FP16，与旧基线（96.3% 成功率）一致
#   - 无需 --fp16 flag (TRT 11.1 的 trtexec 已移除该选项), 网络全 FP16 由 ONNX 定义
#   - 固定 shape (min=opt=max): 动态 profile 在 sm_120 触发 TRT 11 autotuner bug
#   - 对齐 128: lite 模型内部要求 128 倍数 (非 64; 720->768/1080->1152 均 128 倍数)
#   - 引擎版本必须 <= runtime 版本: TRT 升级后重新构建 (trtexec 跟随系统版本)
#
# 用法: bash build_rife_lite_engine.sh [W] [H] [out.engine]
#   W/H 为 视频原始分辨率 (脚本自行 pad 到 128 倍数); 默认 1920x1080
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
W=${1:-1920}; H=${2:-1080}

# pad 到 128 倍数 (lite 对齐要求)
PW=$(( (W + 127) / 128 * 128 ))
PH=$(( (H + 127) / 128 * 128 ))
OUT=${3:-"$HERE/../../build/tests/fruc/rife_lite_fp16_${PH}x${PW}.engine"}
ONNX="$HERE/../../build/tests/fruc/rife_lite_fp16_all.onnx"

echo "=== building lite engine: ${W}x${H} -> pad ${PW}x${PH} ==="

trtexec --onnx="$ONNX" \
    "--minShapes=input:1x11x${PH}x${PW}" \
    "--optShapes=input:1x11x${PH}x${PW}" \
    "--maxShapes=input:1x11x${PH}x${PW}" \
    --saveEngine="$OUT" 2>&1 | grep -E "\[E\]|FAILED|Total Host Walltime" || true

if [ -f "$OUT" ]; then
    echo "=== engine saved: $OUT ($(stat -c%s "$OUT") bytes) ==="
else
    echo "=== BUILD FAILED ===" >&2
    exit 1
fi
