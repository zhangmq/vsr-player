#!/usr/bin/env bash
# check-deps.sh — Verify build-time dependencies are present (current architecture:
# libmpv patch build + Qt6 client + VFX SDK runtime).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
MISSING=0

check_file() {
    if [ -f "$TP/$1" ] || [ -L "$TP/$1" ]; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — MISSING"
        MISSING=1
    fi
}

check_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — MISSING"
        MISSING=1
    fi
}

# 注：VFX 头文件不需要——vsr_proc.c 用 C 兼容重定义（nvCVImage/nvVideoEffects
# 结构布局 + dlsym，见 vsr_internal.h）——只需运行时 .so（dlopen）。
echo "=== third_party/nvvfx/lib (VFX SDK runtime, 自行准备) ==="
check_file "nvvfx/lib/libnvVFXVideoSuperRes.so"
check_file "nvvfx/lib/libVideoFX.so"
check_file "nvvfx/lib/libNVCVImage.so"

echo ""
echo "=== third_party/ (随仓库分发) ==="
check_file "material-icons/materialdesignicons-webfont.ttf"
check_file "mpv/meson.build"

echo ""
echo "=== 构建工具 ==="
check_cmd meson
check_cmd ninja
# lrelease 可能在 /usr/lib/qt6/bin/（不在 PATH）
if command -v lrelease >/dev/null 2>&1 || [ -x /usr/lib/qt6/bin/lrelease ]; then
    echo "  ✅ lrelease"
else
    echo "  ❌ lrelease — MISSING"
    MISSING=1
fi
if [ -d /opt/cuda/include ]; then
    echo "  ✅ /opt/cuda/include (CUDA headers)"
else
    echo "  ❌ /opt/cuda/include — MISSING (CUDA Toolkit)"
    MISSING=1
fi

echo ""
echo "=== RIFE / TensorRT（引擎构建与插帧） ==="
if command -v trtexec >/dev/null 2>&1; then
    echo "  ✅ trtexec ($(trtexec --version 2>/dev/null | head -1 | grep -oE 'v[0-9.]+' | head -1))"
else
    echo "  ❌ trtexec — MISSING (TensorRT；RIFE 引擎构建用，插帧不可用)"
    MISSING=1
fi
if [ -f /usr/lib/libnvinfer.so.11 ] || ls /usr/lib/libnvinfer.so.11* >/dev/null 2>&1; then
    echo "  ✅ libnvinfer.so.11 (TensorRT runtime)"
else
    echo "  ⚠ libnvinfer.so.11 — 系统 TRT 未检测到（分发 tarball 自带捆绑版，dev 运行需系统 TRT）"
fi

echo ""
if [ "$MISSING" -eq 0 ]; then
    echo "All dependencies present."
    echo "Build: ./scripts/build_mpv.sh && ninja -C build"
    echo "Release: ./scripts/build_release.sh"
else
    echo ""
    echo "Missing dependencies. See docs/third-party-setup.md for setup instructions."
    exit 1
fi
