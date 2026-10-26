#!/bin/bash
# MVC 双视图 → 全宽 SBS MKV（每眼原生 1920×1080，输出 3840×1080）
#
# 依赖：打过 FFmpeg MVC 补丁的源码树（用于编 mvc2sbs）+ 系统 ffmpeg（libx265）
#       + python3（ssif_split_merge.py）+ 7z（仅 ISO 路径需要）
#
# 用法：
#   ./mvc_to_sbs.sh <merged.264> <out.mkv> [crf] [preset]
#   ./mvc_to_sbs.sh <disc.iso>   <out.mkv> [crf] [preset]
#
# ISO 路径目前做到"从 ISO 流式取 SSIF 前 N 秒/整条"这一步为止（见 README 的 TODO：
# 整片需要 UDF 访问或先挂载；挂载后可直接用 BDMV/STREAM/SSIF/xxxxx.ssif 作为输入）。
set -euo pipefail

IN="${1:?输入：merged.264 或 .m2ts/.ssif 或 .iso}"
OUT="${2:?输出 .mkv}"
CRF="${3:-16}"
PRESET="${4:-slow}"
HERE="$(cd "$(dirname "$0")" && pwd)"
MVC2SBS="${MVC2SBS:-$HERE/../../build/mvc2sbs}"

[ -x "$MVC2SBS" ] || { echo "缺少 $MVC2SBS（按 README 用打过补丁的 libavcodec 编译）" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

case "$IN" in
  *.iso)
    echo "[1/3] 从 ISO 取 SSIF（整条，可能需要几十 GB 临时空间）…"
    7z e -so "$IN" "BDMV/STREAM/SSIF/00006.ssif" > "$work/ssif.m2ts"
    echo "[2/3] 拆双视图并按 PTS 配对…"
    python3 "$HERE/ssif_split_merge.py" "$work/ssif.m2ts" "$work/merged.264"
    SRC="$work/merged.264" ;;
  *.m2ts|*.ssif)
    echo "[2/3] 拆双视图并按 PTS 配对…"
    python3 "$HERE/ssif_split_merge.py" "$IN" "$work/merged.264"
    SRC="$work/merged.264" ;;
  *)
    SRC="$IN" ;;
esac

echo "[3/3] 解码两视图 → 拼 SBS → x265(crf=$CRF, preset=$PRESET, 10-bit)…"
# 帧率/尺寸需与片源一致：蓝光 3D 常见 23.976 / 24 / 25 / 29.97 fps
FPS="${FPS:-24000/1001}"
"$MVC2SBS" "$SRC" \
  | ffmpeg -hide_banner -f rawvideo -pix_fmt yuv420p -s 3840x1080 -r "$FPS" -i - \
           -c:v libx265 -preset "$PRESET" \
           -x265-params "crf=$CRF:log-level=error:aq-mode=3" \
           -pix_fmt yuv420p10le -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
           -y "$OUT"
echo "完成：$OUT"
