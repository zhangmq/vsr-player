#!/bin/bash
# Build custom mpv with vf_vsr + Wayland external surface support.
#
# Patched files in src/mpv/ overlay the pristine third_party/mpv/ source.
# Build output: build/mpv/

set -euo pipefail

# 脚本在 scripts/ 下——项目根 = 脚本目录上一级
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MPV_REF="$PROJECT_ROOT/third_party/mpv"
OVERLAY="$PROJECT_ROOT/src/mpv"
BUILD_DIR="$PROJECT_ROOT/build/mpv"

echo "=== Preparing mpv build tree ==="
rm -rf "$BUILD_DIR"
cp -a "$MPV_REF" "$BUILD_DIR"
cp -a "$OVERLAY"/* "$BUILD_DIR"

echo "=== Configuring ==="
cd "$BUILD_DIR"
meson setup _build \
    -Dlibmpv=true \
    -Dcplayer=true \
    -Dvulkan=enabled \
    -Dwayland=enabled \
    -Dgpl=true \
    -Dcuda-hwaccel=enabled \
    -Dcuda-interop=enabled \
    --buildtype=debug \
    --wipe

echo "=== Building ==="
ninja -C _build

echo "=== Done ==="
echo "mpv:    $BUILD_DIR/_build/mpv"
echo "libmpv: $BUILD_DIR/_build/libmpv.so"
