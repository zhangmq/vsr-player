#!/usr/bin/env bash
# 从 ONNX 模型构建 RIFE TensorRT 引擎（固定 512×512 输入）。
# 产物 third_party/rife/rife512.engine 由 install.sh 分发到 ~/.local/lib/vsr-player/。
#
# 依赖：TensorRT（trtexec）。TRT 11 起移除 fp16 构建开关（strongly-typed，
# TF32 默认启用）——fp32 引擎即目标产物。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ONNX="$ROOT/third_party/rife/rife_v4.25.onnx"
OUT="$ROOT/third_party/rife/rife512.engine"

if [ ! -f "$ONNX" ]; then
    echo "✗ $ONNX missing (download vs-mlrt rife_v4.25 v2 conversion)" >&2
    exit 1
fi
if ! command -v trtexec >/dev/null; then
    echo "✗ trtexec not found (install TensorRT)" >&2
    exit 1
fi

trtexec --onnx="$ONNX" \
    --shapes=input:1x7x512x512 \
    --saveEngine="$OUT"

echo "✓ engine → $OUT ($(du -h "$OUT" | cut -f1))"
