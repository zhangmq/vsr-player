#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

INSTALL_DIR="${INSTALL_DIR:-$HOME/vsr-player}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo ""
echo -e "${GREEN}═══════════════════════════════════════${NC}"
echo -e "${GREEN}  VSR Player — Installer${NC}"
echo -e "${GREEN}═══════════════════════════════════════${NC}"
echo ""
echo "Install to: ${YELLOW}$INSTALL_DIR${NC}"
echo ""

# ── 1. Check system dependencies ──────────────────────────────────────

check_pkg() {
    if pkg-config --exists "$1" 2>/dev/null; then
        echo -e "  ✅ $1"
    else
        echo -e "  ${RED}❌ $1 — not found${NC}"
        MISSING_SYS=1
    fi
}

echo "Checking system dependencies..."
MISSING_SYS=0

check_pkg Qt6Quick
check_pkg Qt6QuickControls2
check_pkg vulkan
check_pkg libavcodec
check_pkg libavformat
check_pkg libavutil
check_pkg libswscale
check_pkg portaudio-2.0

# libcuda.so.1
if ldconfig -p 2>/dev/null | grep -q "libcuda\.so\.1" || \
   find /usr/lib* -name "libcuda.so.1" 2>/dev/null | grep -q .; then
    echo -e "  ✅ libcuda.so.1"
else
    echo -e "  ${RED}❌ libcuda.so.1 — NVIDIA driver not found${NC}"
    MISSING_SYS=1
fi

if [ "$MISSING_SYS" -ne 0 ]; then
    echo ""
    echo -e "${RED}Missing system dependencies. Install with:${NC}"
    echo ""
    echo "  Arch:   sudo pacman -S qt6-base ffmpeg vulkan-devel portaudio"
    echo "  Ubuntu: sudo apt install qt6-base-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libvulkan-dev portaudio19-dev"
    exit 1
fi

echo -e "  ${GREEN}All system dependencies present.${NC}"

# ── 2. Create directories ──────────────────────────────────────────────

mkdir -p "$INSTALL_DIR"/{bin,lib/fonts,share/vsr-player/{qml,shaders},screenshots,config}

# ── 3. Copy binary + bundled resources ─────────────────────────────────

echo ""
echo "Copying application files..."

# Binary
if [ -f "$SRC_DIR/build/vsr-player" ]; then
    cp "$SRC_DIR/build/vsr-player" "$INSTALL_DIR/bin/"
    echo "  ✅ vsr-player (from build/)"
elif [ -f "$SRC_DIR/bin/vsr-player" ]; then
    cp "$SRC_DIR/bin/vsr-player" "$INSTALL_DIR/bin/"
    echo "  ✅ vsr-player (from bin/)"
else
    echo -e "  ${RED}❌ vsr-player binary not found${NC}"
    exit 1
fi

# QML components
cp "$SRC_DIR/src/client/ui/overlay.qml" "$INSTALL_DIR/share/vsr-player/qml/"
cp "$SRC_DIR/src/client/ui"/*.qml "$INSTALL_DIR/share/vsr-player/qml/" 2>/dev/null || true
cp -r "$SRC_DIR/src/client/ui/components" "$INSTALL_DIR/share/vsr-player/qml/" 2>/dev/null || true
echo "  ✅ QML components"

# Shaders
cp "$SRC_DIR/build"/*.vert.spv "$INSTALL_DIR/share/vsr-player/shaders/" 2>/dev/null || true
cp "$SRC_DIR/build"/*.frag.spv "$INSTALL_DIR/share/vsr-player/shaders/" 2>/dev/null || true
echo "  ✅ Shaders"

# Font
cp "$SRC_DIR/third_party/fonts/MaterialIcons-Regular.ttf" "$INSTALL_DIR/lib/fonts/"
echo "  ✅ Font"

# ── 4. Install nvidia-vfx runtime ──────────────────────────────────────

echo ""
echo "Installing NVIDIA VFX runtime..."

PYTHON=""
# Search: conda/mamba env first, then system
for py_base in \
    "$CONDA_PREFIX/bin/python3" \
    "$MAMBA_ROOT_PREFIX/bin/python3" \
    "$HOME/miniforge3/bin/python3" \
    "$HOME/mambaforge/bin/python3" \
    /usr/bin/python3 \
    /usr/local/bin/python3; do
    if [ -x "$py_base" ] && "$py_base" -m pip --version &>/dev/null 2>&1; then
        PYTHON="$py_base"
        break
    fi
done

# Fallback: walk PATH
if [ -z "$PYTHON" ]; then
    for py in python3 python; do
        py_path=$(command -v "$py" 2>/dev/null || true)
        if [ -n "$py_path" ] && "$py_path" -m pip --version &>/dev/null 2>&1; then
            PYTHON="$py_path"
            break
        fi
    done
fi

if [ -z "$PYTHON" ]; then
    echo -e "  ${RED}❌ python3 with pip not found. Install Python and pip first.${NC}"
    exit 1
fi

echo "  → $PYTHON -m pip install nvidia-vfx"
$PYTHON -m pip install nvidia-vfx 2>&1 | tail -1

# Find the installed package's libs directory via pip show
VFX_LOCATION=$($PYTHON -m pip show nvidia-vfx 2>/dev/null | grep "^Location:" | awk '{print $2}')

if [ -n "$VFX_LOCATION" ] && [ -d "$VFX_LOCATION/nvvfx/libs" ]; then
    VFX_LIB_DIR="$VFX_LOCATION/nvvfx/libs"
    cp "$VFX_LIB_DIR"/*.so* "$INSTALL_DIR/lib/"
    echo -e "  ${GREEN}✅ VFX runtime copied from $VFX_LIB_DIR${NC}"
else
    echo -e "  ${RED}❌ Could not find nvidia-vfx libs/ directory${NC}"
    echo "  Try: $PYTHON -m pip install nvidia-vfx"
    exit 1
fi

# ── 5. Copy CUDA NVRTC libraries ───────────────────────────────────────

echo ""
echo "Locating CUDA NVRTC libraries..."

NVRTC_FOUND=0
for cuda_dir in \
    /opt/cuda/lib64 \
    /opt/cuda/targets/x86_64-linux/lib \
    /usr/local/cuda/lib64 \
    /usr/local/cuda/targets/x86_64-linux/lib \
    "$HOME/cuda_pkg/opt/cuda/targets/x86_64-linux/lib"; do
    if [ -f "$cuda_dir/libnvrtc.so.13" ]; then
        cp "$cuda_dir/libnvrtc.so.13" "$INSTALL_DIR/lib/"
        cp "$cuda_dir/libnvrtc-builtins.so.13.0" "$INSTALL_DIR/lib/"
        echo -e "  ${GREEN}✅ NVRTC from $cuda_dir${NC}"
        NVRTC_FOUND=1
        break
    fi
done

# Check pip-installed CUDA packages (nvidia-cu13, etc.)
if [ "$NVRTC_FOUND" -eq 0 ]; then
    CUDA_PIP_LIB=$($PYTHON -c "
import os, site
# Search all site-packages dirs for nvidia/cu*/lib/
for sp in site.getsitepackages() + [site.getusersitepackages()]:
    for root, dirs, files in os.walk(sp):
        if os.path.basename(root) == 'lib' and 'nvidia' in root and 'libnvrtc.so.13' in files:
            print(root)
            raise StopIteration
" 2>/dev/null)

    if [ -n "$CUDA_PIP_LIB" ]; then
        cp "$CUDA_PIP_LIB/libnvrtc.so.13" "$INSTALL_DIR/lib/"
        cp "$CUDA_PIP_LIB/libnvrtc-builtins.so.13.0" "$INSTALL_DIR/lib/"
        echo -e "  ${GREEN}✅ NVRTC from pip CUDA package${NC}"
        NVRTC_FOUND=1
    fi
fi

if [ "$NVRTC_FOUND" -eq 0 ]; then
    echo -e "  ${RED}❌ libnvrtc.so.13 not found${NC}"
    echo "  Install with: sudo pacman -S cuda"
    echo "  Or: $PYTHON -m pip install nvidia-cuda-runtime-cu13"
    exit 1
fi

# ── 6. Create .so symlinks ─────────────────────────────────────────────

echo ""
echo "Creating library symlinks..."
cd "$INSTALL_DIR/lib"

# NVRTC
ln -sf libnvrtc.so.13 libnvrtc.so 2>/dev/null || true
ln -sf libnvrtc-builtins.so.13.0 libnvrtc-builtins.so 2>/dev/null || true

# VFX chain — map lib.<name>.so.<ver> → lib<name>.so
for lib in libnvidia-ngx-vsr libnppc libnppial libnppicc libnppidei \
           libnppif libnppig libnppim libnppist libnppitc \
           libnvinfer libnvinfer_plugin libnvonnxparser libcudnn; do
    target=$(ls ${lib}.so.* 2>/dev/null | head -1)
    [ -n "$target" ] && ln -sf "$(basename "$target")" "${lib}.so" 2>/dev/null || true
done

echo -e "  ${GREEN}✅ Symlinks created${NC}"

# ── 7. Verify installation ─────────────────────────────────────────────

echo ""
echo "Verifying installation..."

MISSING_LIB=0
check_lib() {
    if [ -f "$INSTALL_DIR/lib/$1" ] || [ -L "$INSTALL_DIR/lib/$1" ]; then
        echo "  ✅ $1"
    else
        echo -e "  ${RED}❌ $1 — MISSING${NC}"
        MISSING_LIB=1
    fi
}

check_lib "libnvVFXVideoSuperRes.so"
check_lib "libVideoFX.so"
check_lib "libnvrtc.so.13"
check_lib "libnvrtc.so"
check_lib "fonts/MaterialIcons-Regular.ttf"

if [ "$MISSING_LIB" -ne 0 ]; then
    echo -e "${RED}Some libraries are missing.${NC}"
    exit 1
fi

echo -e "  ${GREEN}All files verified.${NC}"

# ── 8. Done ────────────────────────────────────────────────────────────

echo ""
echo -e "${GREEN}═══════════════════════════════════════${NC}"
echo -e "${GREEN}  Installation complete!${NC}"
echo -e "${GREEN}═══════════════════════════════════════${NC}"
echo ""
echo "  Binary:   $INSTALL_DIR/bin/vsr-player"
echo "  Libs:     $INSTALL_DIR/lib/"
echo "  Data:     $INSTALL_DIR/share/vsr-player/"
echo "  Screenshots: $INSTALL_DIR/screenshots/"
echo ""
echo "Add to shell config (~/.zshrc or ~/.bashrc):"
echo ""
echo -e "  ${YELLOW}export PATH=\"\$PATH:$INSTALL_DIR/bin\"${NC}"
echo ""
echo "Then run:"
echo ""
echo -e "  ${YELLOW}vsr-player <video_file_or_folder>${NC}"
echo ""
