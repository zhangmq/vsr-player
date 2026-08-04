#!/bin/bash
# Install the standalone mpv-vsr CLI (custom mpv with vf_vsr) to ~/.local.
# Run from project root or from scripts/.
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$HOME/.local/bin"
LIB_DIR="$HOME/.local/lib/vsr-player"

echo "=== Installing mpv-vsr ==="

# 1. Install binary
echo "  → $BIN_DIR/mpv-vsr"
mkdir -p "$BIN_DIR"
cp "$PROJECT_ROOT/build/mpv/_build/mpv" "$BIN_DIR/mpv-vsr"
chmod +x "$BIN_DIR/mpv-vsr"

# 2. Install VFX SDK libs（vsr_proc.c 运行时搜索路径含 ~/.local/lib/vsr-player/）
echo "  → $LIB_DIR/"
mkdir -p "$LIB_DIR"
cp "$PROJECT_ROOT/third_party/nvvfx/lib/"* "$LIB_DIR/"

# 3. Install wrapper (uses mpv-vsr internally)
echo "  → $BIN_DIR/mpv-vsr-wrapper.py"
cp "$PROJECT_ROOT/src/scripts/mpv-vsr-wrapper.py" "$BIN_DIR/mpv-vsr-wrapper.py"
chmod +x "$BIN_DIR/mpv-vsr-wrapper.py"

echo "=== Done ==="
echo "Usage:"
echo "  mpv-vsr --vf=vsr:scale=2 <video>        # direct call with VSR"
echo "  mpv-vsr-wrapper.py <url>                  # wrapper with auto-VSR"
