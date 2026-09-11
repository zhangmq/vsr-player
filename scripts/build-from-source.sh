#!/usr/bin/env bash
# build-from-source.sh — 一条命令完成"源码构建 + 本地安装"（滚动发行版用户路径）。
#
# 定位：不下载预编译包、直接在自己机器上构建——产出的二进制天然匹配本机
# 系统库（Qt/FFmpeg/媒体栈），因此系统升级后只需重跑本脚本即可恢复。
# 预编译包用户请用 GitHub Releases 的 tarball（standard / full 两个变体）。
#
# 用法:
#   ./scripts/build-from-source.sh              # 自检 → 构建 → 安装 → 记录快照
#   ./scripts/build-from-source.sh --no-install # 只构建，不安装
#   ./scripts/build-from-source.sh --no-vfx     # 安装时跳过 VFX 自动下载（已在别处备好）
#   ./scripts/build-from-source.sh --jobs 8     # 指定并行度
#
# 前置：meson、ninja、gcc/clang、Qt6 开发包（≥6.11）、FFmpeg 9.x 开发包、
#       CUDA Toolkit（/opt/cuda）、TensorRT（trtexec）；VFX SDK 运行时见
#       docs/third-party-setup.md（首次可让 install.sh 自动下载，约 0.6 GB）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

JOBS="$(nproc 2>/dev/null || echo 4)"
DO_INSTALL=1
INSTALL_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-install) DO_INSTALL=0 ;;
        --no-vfx) INSTALL_ARGS+=(--no-vfx) ;;
        --jobs) JOBS="${2:?--jobs 需要参数}"; shift ;;
        --jobs=*) JOBS="${1#--jobs=}" ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "❌ 未知参数: $1（-h 查看用法）" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
fail() { printf '\n❌ %s\n' "$*" >&2; exit 1; }
elapsed() { echo "$(( $(date +%s) - START ))s"; }
START="$(date +%s)"

step "[1/5] 依赖自检"
if ! bash scripts/check-deps.sh; then
    fail "依赖自检未通过。修掉上方 ❌ 后重跑；系统库升级导致的漂移会在 [5/5] 自动记录。"
fi

step "[2/5] 构建 libmpv（纯净 mpv 0.41 基座 + src/mpv 覆盖层合并）"
if ! bash scripts/build_mpv.sh; then
    fail "libmpv 构建失败（build_mpv.sh 输出经 grep 过滤，失败信息可能被吞——可手动重跑看完整输出）"
fi
LIBMPV="build/mpv/_build/libmpv.so.2"
[ -f "$LIBMPV" ] || LIBMPV="build/mpv/_build/libmpv.so"
[ -f "$LIBMPV" ] || fail "libmpv 未生成（$LIBMPV 不存在）"
echo "  ✅ $LIBMPV"

step "[3/5] 构建客户端（Qt6）"
if [ ! -f build/build.ninja ]; then
    echo "  meson 尚未配置——先 meson setup build"
    meson setup build || fail "meson setup 失败（缺 Qt6/FFmpeg 开发包？见上方自检）"
fi
ninja -C build -j "$JOBS" || fail "客户端构建失败"
[ -x build/src/client/vsr-player ] || fail "客户端二进制未生成"
echo "  ✅ build/src/client/vsr-player"

step "[4/5] 安装到 ~/.local"
if [ "$DO_INSTALL" -eq 1 ]; then
    bash scripts/install.sh "${INSTALL_ARGS[@]}" || fail "安装失败"
else
    echo "  （--no-install：跳过）"
fi

step "[5/5] 记录依赖快照（供 check-deps.sh 检测系统升级导致的漂移）"
# 快照记录"构建时解析到的库文件（路径+mtime+size）"；日后系统升级替换了
# 其中任何一个，check-deps.sh 会提示重建——这正是源码路径相对预编译包的
# 优势：你能立刻用同一条命令修好。
bash scripts/check-deps.sh --record || true

printf '\n\033[1m✅ 构建完成（%s）\033[0m\n' "$(elapsed)"
echo "运行: vsr-player <视频或目录>"
echo "  （若 ~/.local/bin 不在 PATH: export PATH=\"\$PATH:\$HOME/.local/bin\"）"
echo "系统升级后: ./scripts/build-from-source.sh   # 一条命令重建"
