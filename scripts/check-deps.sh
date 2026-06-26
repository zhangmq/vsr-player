#!/usr/bin/env bash
# check-deps.sh — Verify build-time dependencies are present.
# Called automatically by Makefile before compilation.
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

echo "=== Checking third_party/nvvfx/ (headers) ==="
check_file "nvvfx/include/nvCVImage.h"
check_file "nvvfx/include/nvCVStatus.h"
check_file "nvvfx/include/nvVideoEffects.h"

echo ""
echo "=== Checking third_party/fonts/ ==="
check_file "fonts/MaterialIcons-Regular.ttf"

echo ""
if [ "$MISSING" -eq 0 ]; then
    echo "All dependencies present."
else
    echo ""
    echo "Missing dependencies. See docs/BUILD.md for setup instructions."
    exit 1
fi
