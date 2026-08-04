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

echo "=== third_party/nvvfx/ (VFX SDK headers + runtime, 自行准备) ==="
check_file "nvvfx/include/nvCVImage.h"
check_file "nvvfx/include/nvCVStatus.h"
check_file "nvvfx/include/nvVideoEffects.h"
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
if [ "$MISSING" -eq 0 ]; then
    echo "All dependencies present."
    echo "Build: ./scripts/build_mpv.sh && ninja -C build"
else
    echo ""
    echo "Missing dependencies. See docs/third-party-setup.md for setup instructions."
    exit 1
fi
