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
MPV_PIN="${MPV_PIN:-b0841e8c4d156b8403dc1876c4cbd9011eb583f3}"   # v0.42.0
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
# FRUC_BACKEND（插帧后端，默认 vfg）：注入 overlay 选项 -Dfruc_backend=…
# 选项定义在 src/mpv/meson.options（overlay 新增），combo vfg|rife，默认 vfg。
# 与 DIST_RPATH 同款条件注入：显式置空时不传，交给 mpv 侧默认值。
# build_release.sh / build-from-source.sh 经环境变量传值。
FRUC_BACKEND="${FRUC_BACKEND:-vfg}"
# NO_RTX=1：不编 VFX/CUDA 滤镜（vf_hwup/vf_rot/vf_eye/vf_vsr/vf_vfg/vf_truehdr），
# libmpv 不再 NEEDED libcuda/libnvrtc —— 给没有 NVIDIA 驱动的机器用（客户端照常
# 跑，AI 节点不在链上 = 恒直通）。渲染层（Vulkan render API）仍然编入，否则
# 客户端连窗口都画不出来（见 src/mpv/include/mpv/render_vk.h）。
NO_RTX="${NO_RTX:-0}"
if [ "$NO_RTX" = 1 ]; then
    CUDA_ARGS=(-Dno_rtx=true -Dcuda-hwaccel=disabled -Dcuda-interop=disabled)
    echo "=== no-rtx: VFX/CUDA filters disabled (AI unavailable, passthrough only) ==="
else
    CUDA_ARGS=(-Dcuda-hwaccel=enabled -Dcuda-interop=enabled)
fi
meson setup _build \
    -Dlibmpv=true \
    -Dcplayer=true \
    -Dvulkan=enabled \
    -Dwayland=enabled \
    -Dgpl=true \
    "${CUDA_ARGS[@]}" \
    --buildtype="$BUILDTYPE" \
    ${DIST_RPATH:+-Ddist-rpath="$DIST_RPATH"} \
    ${FRUC_BACKEND:+-Dfruc_backend="$FRUC_BACKEND"} \
    --wipe

echo "=== Building ==="
ninja -C _build

echo "=== Done ==="
echo "mpv:    $BUILD_DIR/_build/mpv"
echo "libmpv: $BUILD_DIR/_build/libmpv.so"
