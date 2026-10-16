#!/usr/bin/env bash
# render-icon.sh — 由 data/vsr-player.svg 生成 data/vsr-player.png（512×512）。
#
# 图标**当前是占位**（用户 2026-10-17：后续做像样的）：换图标时改
# data/vsr-player.svg（矢量，单一事实源）后跑本脚本重生成 PNG；install.sh 会把
# svg 装到 hicolor/scalable/apps、png 装到 hicolor/512x512/apps（DE 自选）。
# 只换 PNG（没有 SVG）也可以：直接把 data/vsr-player.png 覆盖成 512×512 即可。
set -euo pipefail
cd "$(dirname "$0")/.."

SVG="data/vsr-player.svg"
PNG="data/vsr-player.png"
[ -f "$SVG" ] || { echo "❌ 缺 $SVG" >&2; exit 1; }

if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 512 -h 512 "$SVG" -o "$PNG"
elif command -v inkscape >/dev/null 2>&1; then
    inkscape "$SVG" -w 512 -h 512 -o "$PNG"
elif command -v magick >/dev/null 2>&1; then
    magick -background none -density 384 "$SVG" -resize 512x512 "$PNG"
else
    echo "❌ 需要 rsvg-convert / inkscape / magick 之一来渲染 SVG" >&2
    exit 1
fi
echo "✅ $PNG（$(du -h "$PNG" | cut -f1)）← $SVG"
