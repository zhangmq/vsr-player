#!/usr/bin/env bash
# build_release.sh — 构建可分发的 vsr-player release tarball。
#
# 产物结构（tarball 与安装目标同构，$ORIGIN 相对 RPATH 两边都成立）：
#   vsr-player-<ver>/
#   ├── vsr-player            # GUI 二进制（RUNPATH=$ORIGIN/../lib/vsr-player）
#   ├── mpv-vsr               # CLI 二进制（RPATH=$ORIGIN/../lib/vsr-player）
#   ├── lib/vsr-player/       # 安装 → ~/.local/lib/vsr-player/（与 VFX 汇合）
#   │   ├── libmpv.so.2       #    libmpv + 捆绑依赖（RPATH 同款）
#   │   ├── ffmpeg ×7         #    防系统版本漂移（62→63 实测破坏）
#   │   ├── libnvrtc/builtins/cudart.so.13   # CUDA runtime（mpv 硬依赖）
#   │   └── libnvinfer(+plugin).so.11        # TRT 11.2.1.2（engine 版本绑定）
#   ├── engines/rife_full_fp16.engine  # ampere+ 跨架构（30/40/50 系通用）
#   ├── fonts/  translations/  licenses/  README.md
#   └── install.sh            # 用户端安装（见 scripts/install.sh）
#
# 流程：mpv dist 构建 → client dist 构建 → 依赖收集 → engine → 资产 → tarball
# 依赖：meson/ninja/trtexec（系统 TensorRT）/lrelease（Qt6 翻译）
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

VERSION="$(grep -oP "version: '\K[^']+" meson.build)"
DIST_RPATH='$ORIGIN/../lib/vsr-player'
MPV_DIST="$PROJECT_ROOT/build/mpv-dist"
CLIENT_DIST="$PROJECT_ROOT/build/client-dist"
STAGE="$PROJECT_ROOT/build/release-staging"
TARBALL="$PROJECT_ROOT/build/vsr-player-$VERSION-linux-x86_64.tar.xz"

echo "=== vsr-player release build (v$VERSION) ==="

# ── 1. mpv dist 构建（独立树 + release + dist-rpath）──────────────────
echo "--- [1/6] mpv (dist build) ---"
MPV_BUILD_DIR="$MPV_DIST" DIST_RPATH="$DIST_RPATH" BUILDTYPE=release \
    ./scripts/build_mpv.sh

# ── 2. client dist 构建 ──────────────────────────────────────────────
echo "--- [2/6] client (dist build) ---"
rm -rf "$CLIENT_DIST"
meson setup "$CLIENT_DIST" --buildtype=release \
    -Ddist-rpath="$DIST_RPATH" \
    -Dmpv-build-dir="$MPV_DIST/_build" >/dev/null
ninja -C "$CLIENT_DIST"

# ── 3. 组装目录 ─────────────────────────────────────────────────────
echo "--- [3/6] staging ---"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/vsr-player" "$STAGE/engines" "$STAGE/fonts" \
         "$STAGE/translations" "$STAGE/licenses"
cp "$CLIENT_DIST/src/client/vsr-player" "$STAGE/vsr-player"
cp "$MPV_DIST/_build/mpv" "$STAGE/mpv-vsr"
cp -L "$MPV_DIST/_build/libmpv.so.2" "$STAGE/lib/vsr-player/libmpv.so.2"

# ── 4. 依赖库收集（ldd 解析路径；校验 SONAME 文件名）────────────────
echo "--- [4/6] bundled libs ---"
BIN_REF="$MPV_DIST/_build/mpv"
collect() {  # collect <libdir> <soname-glob> [extra-path]
    local dir="$1" pat="$2" extra="${3:-}"
    local found=""
    if [ -n "$extra" ] && [ -d "$extra" ]; then
        found="$(find "$extra" -name "$pat" -type f | head -1)"
    fi
    [ -z "$found" ] && found="$(ldd "$BIN_REF" | awk -v p="$pat" '$1==p {print $3}' | head -1)"
    if [ -z "$found" ] || [ ! -f "$found" ]; then
        echo "  ❌ $pat not found" >&2; return 1
    fi
    # 复制为 SONAME 文件名（解引用软链——单一文件即可满足加载器按名查找）
    cp -L "$found" "$dir/$pat"
    echo "  ✓ $pat ($(basename "$found"))"
}
LIB="$STAGE/lib/vsr-player"
for f in libavcodec.so.63 libavformat.so.63 libavutil.so.61 libavfilter.so.12 \
         libswscale.so.10 libswresample.so.7 libavdevice.so.63; do
    collect "$LIB" "$f"
done
collect "$LIB" libnvrtc.so.13 "" /opt/cuda/lib64
collect "$LIB" libnvrtc-builtins.so.13 "" /opt/cuda/lib64
collect "$LIB" libcudart.so.13 "" /opt/cuda/lib64
collect "$LIB" libnvinfer.so.11
collect "$LIB" libnvinfer_plugin.so.11 || echo "  ⚠ plugin missing（RIFE 图无 plugin 算子，可缺）"

# ── 5. engine + 资产 ────────────────────────────────────────────────
echo "--- [5/6] engine + assets ---"
bash tests/fruc/build_rife_full_engine.sh --hardware-compat=on \
    "$STAGE/engines/rife_full_fp16.engine"
cp third_party/material-icons/materialdesignicons-webfont.ttf "$STAGE/fonts/"
cp "$CLIENT_DIST/src/client/"*.qm "$STAGE/translations/"

# ── 6. 许可 + README + tarball ──────────────────────────────────────
echo "--- [6/6] licenses + tarball ---"
# NVIDIA 许可文本（随软件分发的许可要求）；来源：nvidia-vfx wheel 内附
#（PyPI 官方包）。本地 pip 缓存缺失时跳过（README 引用官方 URL）。
WHEEL="$(find "$HOME/.cache/pip" -name "nvidia_vfx*.whl" 2>/dev/null | head -1)"
if [ -n "$WHEEL" ]; then
    unzip -o -q "$WHEEL" \
        "nvidia_vfx-*/dist-info/licenses/packaging/*" -d "$STAGE/licenses/" \
        2>/dev/null || true
    # 扁平化（去掉 dist-info 前缀目录）
    find "$STAGE/licenses" -mindepth 2 -type f -exec mv {} "$STAGE/licenses/" \;
    find "$STAGE/licenses" -type d -empty -delete
    echo "  ✓ NVIDIA SLA (from nvidia-vfx wheel)"
else
    echo "  ⚠ nvidia-vfx wheel not in pip cache — licenses/ 缺 SLA 文本（README 有链接）"
fi
cat > "$STAGE/README.md" << 'EOR'
# vsr-player vVERSION_PLACEHOLDER

NVIDIA GPU 实时 AI 超分 + 插帧视频播放器（libmpv + Qt6 + VFX SDK + RIFE）。

## 系统依赖（必须）
- NVIDIA 显卡 + 驱动（libcuda.so.1）——唯一强制外部依赖
- Qt ≥ 6.11（GUI 版；CLI 版不需要）

## 安装
```
./install.sh
```
安装到 ~/.local（bin + lib/vsr-player），不修改任何系统配置。

## VFX SDK（超分引擎，首次安装需下载）
tarball 不含 VFX SDK（NVIDIA SLA 限制再分发）。install.sh 检测缺失时
从 PyPI 官方 nvidia-vfx 包下载并提取（或手动下载后重跑脚本）：
- 官方来源: https://pypi.org/project/nvidia-vfx/
- 许可: NVIDIA Software License Agreement（licenses/，下载前脚本会提示）

## 插帧（RIFE）
engines/rife_full_fp16.engine 为 ampere+ 跨架构构建（Ampere 30 系及以上）。
RTX 20 系（Turing）及更早不支持 → 插帧自动直通（VSR 不受影响）。
插帧需 GPU 支持 FP16 Tensor Cores。

## 运行
```
vsr-player [视频或目录]      # GUI
mpv-vsr --vf=vsr:scale=2 <视频>   # CLI（VSR 2x）
```
EOR
sed -i "s/VERSION_PLACEHOLDER/$VERSION/" "$STAGE/README.md"
cp scripts/install.sh "$STAGE/install.sh" && chmod +x "$STAGE/install.sh"

tar -C "$PROJECT_ROOT/build" -cJf "$TARBALL" "release-staging" \
    --transform "s/release-staging/vsr-player-$VERSION/"
echo ""
echo "=== Done: $TARBALL ($(du -h "$TARBALL" | cut -f1)) ==="
