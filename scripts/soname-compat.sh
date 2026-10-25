#!/usr/bin/env bash
# soname-compat.sh — 缺 soname 时的软链接兜底（**带验证**）。
#
# 场景：standard 包不捆 FFmpeg/CUDA/Qt，二进制的 NEEDED 记的是构建时的 soname
# （如 libavcodec.so.63）。系统 ffmpeg 大版本升级后那份 soname 不存在了 →
# 程序连启动都做不到。
#
# 做法：在 $LIB_DIR（已在 RPATH 里，优先于系统路径）建 compat 软链
#   libavcodec.so.63 -> /usr/lib/libavcodec.so.64
# 然后**用 ldd -r 验证**：只有"无 undefined symbol"才保留软链。
# 为什么必须验证：ffmpeg 跨 major 不保证 ABI，硬指过去可能缺符号——那种情况
# 加载器会直接报 undefined symbol，比"启动成功但功能悄悄坏掉"好得多。
#
# 用法：
#   soname-compat.sh <binary> [<binary>...]     # 修 + 验证
#   soname-compat.sh --dry-run <binary>...      # 只报告缺什么、候选是谁
#
# 退出码：0 = 全部可解析（含 compat 软链通过验证）；1 = 有无法解决的缺失。
set -u

LIB_DIR="${VSR_LIB_DIR:-$HOME/.local/lib/vsr-player}"
DRY=0
[ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }

[ $# -ge 1 ] || { echo "usage: $0 [--dry-run] <binary>..." >&2; exit 2; }

# 检查/验证一律用"$LIB_DIR 在最前"的搜索路径：真实安装里它在二进制的 RPATH 里，
# 但这里不依赖 RPATH 是否存在（兼容/RPATH 被改过的情况也要能验证）。
ldd_env() {
    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        LD_LIBRARY_PATH="$LIB_DIR:$LD_LIBRARY_PATH" ldd "$@"
    else
        LD_LIBRARY_PATH="$LIB_DIR" ldd "$@"
    fi
}
missing_of() {  # 列出 <binary> 缺失的 NEEDED（basename 形式）
    ldd_env "$1" 2>/dev/null | awk '/not found/ {print $1}' | sort -u
}

# 在系统里找同库的其它 major（ldconfig -p 覆盖 /usr/lib、/opt/cuda 等 ld.so.conf 路径；
# 另外扫 LD_LIBRARY_PATH，覆盖 CUDA/自建库那种不在缓存里的安装）
candidate_for() {  # candidate_for <soname> -> 打印候选绝对路径
    local so="$1" base p d f
    base="${so%%.so.*}"                          # libavcodec
    # 1) 同名 soname 已在缓存里（只是没在默认路径上）→ 直接用它
    p="$(ldconfig -p 2>/dev/null | awk -v n="$so" '$1==n {print $NF; exit}')"
    [ -n "$p" ] && [ -e "$p" ] && { printf '%s\n' "$p"; return 0; }
    # 2) 同 base 的**其它 major**（ABI 赌注，由 ldd -r 验证；取 major 最大者）
    {
        ldconfig -p 2>/dev/null \
            | awk -v b="$base" '$1 ~ ("^" b "\\.so\\.[0-9]+$") { print $1 "\t" $NF }'
        local IFS=:
        for d in ${LD_LIBRARY_PATH:-}; do
            [ -d "$d" ] || continue
            for f in "$d/$base".so.[0-9]*; do
                [ -f "$f" ] || continue
                printf '%s\t%s\n' "$(basename "$f")" "$f"
            done
        done
    } | awk -F'\t' '{ n=split($1,a,"."); print a[n] "\t" $2 }' \
      | sort -n | tail -1 | cut -f2-
}

RC=0
CREATED=()
for bin in "$@"; do
    [ -x "$bin" ] || continue
    MISSING="$(missing_of "$bin")"
    [ -z "$MISSING" ] && continue

    echo "  缺 soname: $(printf '%s ' $MISSING)"
    for so in $MISSING; do
        CAND="$(candidate_for "$so")"
        if [ -z "$CAND" ] || [ ! -e "$CAND" ]; then
            echo "    ❌ $so：系统里没有同库的任何版本 → 无法兼容" >&2
            RC=1; continue
        fi
        echo "    → $so 用系统 $CAND 作为兼容替代"
        if [ "$DRY" -eq 1 ]; then
            CREATED+=("$LIB_DIR/$so|$CAND")
            continue
        fi
        mkdir -p "$LIB_DIR"
        ln -sfn "$CAND" "$LIB_DIR/$so"
        CREATED+=("$LIB_DIR/$so|$CAND")
    done
done

[ "$DRY" -eq 1 ] && exit "$RC"
[ ${#CREATED[@]} -eq 0 ] && exit "$RC"

# ── 验证：软链之后不能有 undefined symbol ────────────────────────────
BAD=0
for bin in "$@"; do
    [ -x "$bin" ] || continue
    UNDEF="$(ldd_env -r "$bin" 2>&1 | grep -i 'undefined symbol' | head -5 || true)"
    if [ -n "$UNDEF" ]; then
        echo "  ❌ $bin 用兼容软链后仍缺符号（ABI 不兼容）:" >&2
        printf '%s\n' "$UNDEF" | sed 's/^/       /' >&2
        BAD=1
    fi
    STILL="$(missing_of "$bin")"
    if [ -n "$STILL" ]; then
        echo "  ❌ $bin 仍有未解析的 NEEDED: $STILL" >&2
        BAD=1
    fi
done

if [ "$BAD" -eq 1 ]; then
    # 回滚：不留半生效的兼容软链（否则下次报错更难查）
    for e in "${CREATED[@]}"; do rm -f "${e%%|*}"; done
    echo "  → 已移除本次创建的兼容软链" >&2
    exit 1
fi

for e in "${CREATED[@]}"; do
    echo "  ✅ 兼容软链: ${e%%|*} -> ${e##*|}"
done
echo "  ⚠ 以上是**跨 ABI 兼容**（不是官方匹配）：能加载、符号齐全，但若上游改了"
echo "    调用约定仍可能运行期出错。要彻底免疫请用 full 变体（自带匹配的库）。"
exit 0
