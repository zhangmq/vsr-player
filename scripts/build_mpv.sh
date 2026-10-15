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

# 基座 pin 预检（issue #12 缺陷 2）：src/mpv overlay 是整文件副本，基座 revision
# 不匹配时会以晦涩的编译/链接错误出现。只警告不中止——自行换基座是允许的。
MPV_PIN="${MPV_PIN:-a3d4b5919771a0ae351b80eb266c79681ee529b2}"
if [ -d "$MPV_REF/.git" ]; then
    have="$(git -C "$MPV_REF" rev-parse HEAD 2>/dev/null || true)"
    if [ -n "$have" ] && [ "$have" != "$MPV_PIN" ]; then
        echo "⚠ third_party/mpv 当前 $have ≠ 记录基座 $MPV_PIN"
        echo "  overlay 按该提交制作；基座不同时若编译/链接报错，先 checkout 回它"
        echo "  （见 docs/third-party-setup.md「mpv 源码」）"
    fi
fi

echo "=== Preparing mpv build tree ==="
rm -rf "$BUILD_DIR"
mkdir -p "$(dirname "$BUILD_DIR")"
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
