#!/usr/bin/env bash
# check-deps.sh — Verify all third_party/ dependencies are present.
# Called automatically by Makefile before compilation.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
MISSING=0

msg_ok()  { echo "  ✅ $1"; }
msg_bad() { echo "  ❌ $1"; MISSING=1; }

check_file() {
    if [ -f "$TP/$1" ] || [ -L "$TP/$1" ]; then
        msg_ok "$1"
    else
        msg_bad "$1 — MISSING"
    fi
}

check_dir() {
    if [ -d "$TP/$1" ]; then
        msg_ok "$1/"
    else
        msg_bad "$1/ — MISSING"
    fi
}

echo "=== Checking third_party/cuda/ ==="
check_dir  "cuda/include"
check_file "cuda/include/cuda.h"
check_file "cuda/include/nvrtc.h"
check_dir  "cuda/lib"
check_file "cuda/lib/libnvrtc.so.13"
check_file "cuda/lib/libnvrtc-builtins.so.13.0"

echo ""
echo "=== Checking third_party/nvvfx/ ==="
check_dir  "nvvfx/include"
check_file "nvvfx/include/nvCVImage.h"
check_file "nvvfx/include/nvCVStatus.h"
check_file "nvvfx/include/nvVideoEffects.h"
check_dir  "nvvfx/lib"
check_file "nvvfx/lib/libVideoFX.so"
check_file "nvvfx/lib/libNVCVImage.so"
check_file "nvvfx/lib/libnvVFXVideoSuperRes.so"
check_file "nvvfx/lib/libVideoFXLocal.so"
check_file "nvvfx/lib/libnvngxruntime.so"
check_file "nvvfx/lib/libnvidia-ngx-vsr.so.1.8.2"
check_file "nvvfx/lib/libnvinfer.so.10"
check_file "nvvfx/lib/libnvinfer_plugin.so.10"
check_file "nvvfx/lib/libnvonnxparser.so.10"
check_file "nvvfx/lib/libcudnn.so.9"
check_file "nvvfx/lib/libnppc.so.12"
check_file "nvvfx/lib/libnppial.so.12"
check_file "nvvfx/lib/libnppicc.so.12"
check_file "nvvfx/lib/libnppidei.so.12"
check_file "nvvfx/lib/libnppif.so.12"
check_file "nvvfx/lib/libnppig.so.12"
check_file "nvvfx/lib/libnppim.so.12"
check_file "nvvfx/lib/libnppist.so.12"
check_file "nvvfx/lib/libnppitc.so.12"

echo ""
if [ "$MISSING" -eq 0 ]; then
    echo "All dependencies present."
else
    echo ""
    echo "Some dependencies are missing."
    echo "See third_party/README.md for download instructions."
    exit 1
fi
