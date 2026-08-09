#!/usr/bin/env bash
# install.sh — Install the vsr-player GUI client to ~/.local (user-local).
#
# 现行架构安装：二进制 + VFX SDK 运行时（vsr_proc.c 自动搜索
# ~/.local/lib/vsr-player/）+ 图标字体（main.cpp fallback 路径）+ 翻译。
# QML/shaders 已编译进二进制（qrc），无需复制。
# CLI 版（mpv-vsr）另见 scripts/install_mpv_local.sh。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$HOME/.local/bin"
LIB_DIR="$HOME/.local/lib/vsr-player"
FONT_DIR="$HOME/.local/share/vsr-player/fonts"

echo "=== Installing vsr-player (GUI) to $HOME/.local ==="

# ── 1. 系统依赖检查 ──────────────────────────────────────────────────
MISSING_SYS=0
check_pkg() {
    if pkg-config --exists "$1" 2>/dev/null; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — not found"
        MISSING_SYS=1
    fi
}
echo "Checking system dependencies..."
check_pkg Qt6Quick
check_pkg Qt6QuickControls2
check_pkg vulkan
if ldconfig -p 2>/dev/null | grep -q "libcuda\.so\.1" || \
   find /usr/lib* -name "libcuda.so.1" 2>/dev/null | grep -q .; then
    echo "  ✅ libcuda.so.1"
else
    echo "  ❌ libcuda.so.1 — NVIDIA driver not found"
    MISSING_SYS=1
fi
if [ "$MISSING_SYS" -ne 0 ]; then
    echo "Missing system dependencies (Arch: sudo pacman -S qt6-base vulkan-devel)"
    exit 1
fi

# ── 2. 二进制（dev 构建产物） ────────────────────────────────────────
BIN_SRC="$PROJECT_ROOT/build/src/client/vsr-player"
if [ ! -x "$BIN_SRC" ]; then
    echo "❌ $BIN_SRC not found — run ./scripts/build_mpv.sh && ninja -C build first"
    exit 1
fi
mkdir -p "$BIN_DIR"
cp "$BIN_SRC" "$BIN_DIR/vsr-player"
echo "  ✅ $BIN_DIR/vsr-player"

# ── 3. VFX SDK 运行时（vsr_proc.c 搜索路径含 ~/.local/lib/vsr-player/）─
if [ -d "$PROJECT_ROOT/third_party/nvvfx/lib" ]; then
    mkdir -p "$LIB_DIR"
    cp "$PROJECT_ROOT/third_party/nvvfx/lib/"* "$LIB_DIR/"
    echo "  ✅ VFX runtime → $LIB_DIR/"
else
    echo "  ❌ third_party/nvvfx/lib/ missing (see docs/third-party-setup.md)"
    exit 1
fi

# ── 3.5 RIFE 运行时（vf_rife.c 搜索路径含 ~/.local/lib/vsr-player/）──
# lite 引擎（主引擎，tests/fruc/build_rife_lite_engine.sh 生成到
# build/tests/fruc/）+ 旧实验引擎（third_party/rife/ rife512）全拷贝——
# 不装引擎则 GUI/CLI 在项目目录外启动报 reason=engine 直通。
LITE_ENGINES="$PROJECT_ROOT/build/tests/fruc/rife_lite_fp16_"*.engine
LEGACY_ENGINES="$PROJECT_ROOT/third_party/rife/"*.engine
if ls $LITE_ENGINES >/dev/null 2>&1 || ls $LEGACY_ENGINES >/dev/null 2>&1; then
    mkdir -p "$LIB_DIR"
    cp $LITE_ENGINES "$LIB_DIR/" 2>/dev/null || true
    cp $LEGACY_ENGINES "$LIB_DIR/" 2>/dev/null || true
    echo "  ✅ RIFE engine → $LIB_DIR/"
else
    echo "  ⚠ RIFE engine missing (run tests/fruc/build_rife_lite_engine.sh) — rife passthrough"
fi

# ── 4. 图标字体（main.cpp fallback：<appdir>/../share/vsr-player/fonts/）─
if [ -f "$PROJECT_ROOT/third_party/material-icons/materialdesignicons-webfont.ttf" ]; then
    mkdir -p "$FONT_DIR"
    cp "$PROJECT_ROOT/third_party/material-icons/materialdesignicons-webfont.ttf" "$FONT_DIR/"
    echo "  ✅ Icon font → $FONT_DIR/"
else
    echo "  ❌ materialdesignicons-webfont.ttf missing (see docs/third-party-setup.md)"
    exit 1
fi

# ── 5. 翻译（main.cpp fallback：<appdir>/translations/） ─────────────
if ls "$PROJECT_ROOT/build/src/client"/*.qm >/dev/null 2>&1; then
    mkdir -p "$BIN_DIR/translations"
    cp "$PROJECT_ROOT/build/src/client"/*.qm "$BIN_DIR/translations/"
    echo "  ✅ Translations → $BIN_DIR/translations/"
fi

echo ""
echo "=== Done ==="
echo "  Binary:   $BIN_DIR/vsr-player"
echo "  VFX libs: $LIB_DIR/"
echo "Run: $BIN_DIR/vsr-player <video-or-directory>"
echo "（若 PATH 无 $BIN_DIR，添加：export PATH=\"\$PATH:$BIN_DIR\"）"
echo ""
echo "CLI 版（独立 mpv-vsr）安装：./scripts/install_mpv_local.sh"
