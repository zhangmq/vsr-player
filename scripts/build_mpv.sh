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
BUILD_DIR="${MPV_BUILD_DIR:-$PROJECT_ROOT/build/mpv}"

echo "=== Preparing mpv build tree ==="
rm -rf "$BUILD_DIR"
cp -a "$MPV_REF" "$BUILD_DIR"
cp -a "$OVERLAY"/* "$BUILD_DIR"

echo "=== Configuring ==="
cd "$BUILD_DIR"
# DIST_RPATH（发布打包）：-Ddist-rpath 注入 $ORIGIN 相对捆绑路径；
# BUILDTYPE 可覆盖（默认 debug，发布用 release）
BUILDTYPE="${BUILDTYPE:-debug}"
meson setup _build \
    -Dlibmpv=true \
    -Dcplayer=true \
    -Dvulkan=enabled \
    -Dwayland=enabled \
    -Dgpl=true \
    -Dcuda-hwaccel=enabled \
    -Dcuda-interop=enabled \
    --buildtype="$BUILDTYPE" \
    ${DIST_RPATH:+-Ddist-rpath="$DIST_RPATH"} \
    --wipe

echo "=== Building ==="
ninja -C _build

echo "=== Done ==="
echo "mpv:    $BUILD_DIR/_build/mpv"
echo "libmpv: $BUILD_DIR/_build/libmpv.so"
