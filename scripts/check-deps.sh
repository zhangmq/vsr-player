#!/usr/bin/env bash
# check-deps.sh — Verify build-time dependencies are present (current architecture:
# libmpv patch build + Qt6 client + VFX SDK runtime).
#
# 用法: ./scripts/check-deps.sh [--record]
#   --record  记录当前已构建产物的依赖快照（build/.dep-snapshot.tsv）。
#             每次成功构建后运行一次，作为"依赖漂移"检测的基线。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
MISSING=0
DRIFT=0
RECORD=0

case "${1:-}" in
    --record) RECORD=1 ;;
    "") ;;
    *) echo "用法: $0 [--record]"; exit 2 ;;
esac

check_file() {
    if [ -f "$TP/$1" ] || [ -L "$TP/$1" ]; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — MISSING"
        MISSING=1
    fi
}

check_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1 — MISSING"
        MISSING=1
    fi
}

# 注：VFX 头文件不需要——vsr_proc.c 用 C 兼容重定义（nvCVImage/nvVideoEffects
# 结构布局 + dlsym，见 vsr_internal.h）——只需运行时 .so（dlopen）。
echo "=== third_party/nvvfx/lib (VFX SDK runtime, 自行准备) ==="
check_file "nvvfx/lib/libnvVFXVideoSuperRes.so"
check_file "nvvfx/lib/libVideoFX.so"
check_file "nvvfx/lib/libNVCVImage.so"

echo ""
echo "=== third_party/ (随仓库分发) ==="
check_file "material-icons/materialdesignicons-webfont.ttf"
check_file "mpv/meson.build"

echo ""
echo "=== 构建工具 ==="
check_cmd meson
check_cmd ninja
# lrelease 可能在 /usr/lib/qt6/bin/（不在 PATH）
if command -v lrelease >/dev/null 2>&1 || [ -x /usr/lib/qt6/bin/lrelease ]; then
    echo "  ✅ lrelease"
else
    echo "  ❌ lrelease — MISSING"
    MISSING=1
fi
if [ -d /opt/cuda/include ]; then
    echo "  ✅ /opt/cuda/include (CUDA headers)"
else
    echo "  ❌ /opt/cuda/include — MISSING (CUDA Toolkit)"
    MISSING=1
fi

echo ""
echo "=== RIFE / TensorRT（引擎构建与插帧） ==="
if command -v trtexec >/dev/null 2>&1; then
    echo "  ✅ trtexec ($(trtexec --version 2>/dev/null | head -1 | grep -oE 'v[0-9.]+' | head -1))"
else
    echo "  ❌ trtexec — MISSING (TensorRT；RIFE 引擎构建用，插帧不可用)"
    MISSING=1
fi
if [ -f /usr/lib/libnvinfer.so.11 ] || ls /usr/lib/libnvinfer.so.11* >/dev/null 2>&1; then
    echo "  ✅ libnvinfer.so.11 (TensorRT runtime)"
else
    echo "  ⚠ libnvinfer.so.11 — 系统 TRT 未检测到（分发 tarball 自带捆绑版，dev 运行需系统 TRT）"
fi

echo ""
echo "=== 已构建产物的依赖漂移（系统库升级检测） ==="
# 背景（2026-09-11 事故）：系统媒体库 soname 升级会让已构建的二进制启动即失败——
#   libcdio 2.3.0→2.4.0 (.so.19→.so.21)、libbluray 1.4.1→1.5.0 (.so.3→.so.4)
# 本检查做两件事：① 缺失 soname ② 已解析库文件被替换。
# dev 产物缺失 = 致命（当前构建已被系统升级破坏）；staging 缺失 = 打包树过期（提示，不判失败）。
SNAPSHOT="$ROOT/build/.dep-snapshot.tsv"
DEV_TARGETS=(
    "build/mpv/_build/libmpv.so.2"
    "build/mpv/_build/mpv"
    "build/src/client/vsr-player"
)
STAGE_TARGETS=(
    "build/release-staging/lib/vsr-player/libmpv.so.2"
    "build/release-staging/vsr-player"
    "build/release-staging/mpv-vsr"
    "build/release-staging-full/lib/vsr-player/libmpv.so.2"
    "build/release-staging-full/vsr-player"
    "build/release-staging-full/mpv-vsr"
)
DEP_TARGETS=("${DEV_TARGETS[@]}" "${STAGE_TARGETS[@]}")

# 输出 <target>\t<soname>\t<resolved-path>\t<mtime>\t<size>
# staging 目标：RPATH 是 $ORIGIN/../lib/vsr-player（安装后才成立），未安装态直接
# ldd 会落到系统库——用 LD_LIBRARY_PATH 指向捆绑目录，模拟安装后的解析结果。
ldd_of() {
    local t="$1"
    if [[ "$t" == build/release-staging* ]]; then
        local base="$ROOT/$(printf '%s' "$t" | cut -d/ -f1-2)"
        LD_LIBRARY_PATH="$base/lib/vsr-player:$base/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            ldd "$ROOT/$t" 2>/dev/null
    else
        ldd "$ROOT/$t" 2>/dev/null
    fi
}

snapshot_deps() {
    local t soname path
    # 只对 dev 产物记录 mtime/size：staging 的捆绑库每次打包都会重写 mtime，
    # 纳入快照会产生"库被替换"的误报（staging 只做缺失 soname 检查）。
    for t in "${DEV_TARGETS[@]}"; do
        [ -f "$ROOT/$t" ] || continue
        while IFS= read -r line; do
            soname="$(printf '%s' "$line" | awk '{print $1}')"
            path="$(printf '%s' "$line" | awk '{print $3}')"
            [ -n "$path" ] || continue
            # 只记录**系统库**：仓库内自建产物（libmpv/捆绑库）每次重建都变
            # mtime，纳入比对会在每次构建后误报"库被替换"。
            case "$path" in "$ROOT"/*) continue ;; esac
            printf '%s\t%s\t%s\t%s\t%s\n' "$t" "$soname" "$path" \
                "$(stat -Lc %Y "$path" 2>/dev/null || echo -)" \
                "$(stat -Lc %s "$path" 2>/dev/null || echo -)"
        done < <(ldd_of "$t" | awk '/=>/ && $3 ~ /^\// {print $1, "=>", $3}')
    done
}

BUILT=0
STAGING_STALE=0
for t in "${DEP_TARGETS[@]}"; do
    [ -f "$ROOT/$t" ] || continue
    BUILT=1
    missing="$(ldd_of "$t" | awk '/not found/ {print $1}' | tr '\n' ' ')"
    [ -n "$missing" ] || continue
    if [[ "$t" == build/release-staging/* ]]; then
        echo "  ⚠ $t — 缺失: $missing（打包树过期 → ./scripts/build_release.sh）"
        STAGING_STALE=1
    else
        echo "  ❌ $t — 缺失: $missing（系统库已升级 → ./scripts/build_mpv.sh && ninja -C build）"
        DRIFT=1
    fi
done

if [ "$BUILT" -eq 0 ]; then
    echo "  ⏭  未发现已构建产物（先构建再检查）"
elif [ "$RECORD" -eq 1 ]; then
    snapshot_deps > "$SNAPSHOT"
    echo "  ✅ 已记录依赖快照: build/.dep-snapshot.tsv（$(wc -l < "$SNAPSHOT") 条）"
elif [ -f "$SNAPSHOT" ]; then
    declare -A cur=()
    while IFS=$'\t' read -r tgt soname path mtime size; do
        cur["$tgt|$soname"]="$mtime|$size"
    done < <(snapshot_deps)
    CHANGED=0
    while IFS=$'\t' read -r tgt soname path mtime size; do
        now="${cur["$tgt|$soname"]:-}"
        [ -n "$now" ] || continue          # 缺失情况已在上面报过
        if [ "$now" != "$mtime|$size" ]; then
            echo "  ⚠ $soname 自构建后被替换（$path）"
            CHANGED=1
        fi
    done < "$SNAPSHOT"
    if [ "$CHANGED" -eq 1 ]; then
        echo "     → 系统媒体库有变动，建议重建：./scripts/build_mpv.sh && ninja -C build"
        DRIFT=1
    else
        echo "  ✅ 无漂移（与构建时快照一致）"
    fi
else
    echo "  ⏭  无快照基线——构建成功后运行 ./scripts/check-deps.sh --record 建立"
fi

echo ""
if [ "$MISSING" -eq 1 ]; then
    echo ""
    echo "Missing dependencies. See docs/third-party-setup.md for setup instructions."
    exit 1
elif [ "$DRIFT" -eq 1 ]; then
    echo "系统库与已构建产物不匹配（见上）。重建：./scripts/build_mpv.sh && ninja -C build"
    exit 1
fi

if [ "$STAGING_STALE" -eq 1 ]; then
    echo "⚠ 打包树 build/release-staging 已过期——发布前需重新打包：./scripts/build_release.sh"
fi
echo "All dependencies present."
echo "Build: ./scripts/build_mpv.sh && ninja -C build"
echo "Release: ./scripts/build_release.sh"
