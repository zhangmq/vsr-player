#!/usr/bin/env bash
# gen-components.sh — 生成发布包的「组件版本表」(COMPONENTS.md)。
#
# 用户 2026-10-26 定的发版规则：
#   - **完整版（full）**：携带所有依赖 → 表里逐条列出"本包自带"的库与版本；
#   - **标准版（standard）**：不携带第三方依赖 → 表里逐条列出"要求系统提供"的
#     SONAME 与构建机上的实际版本，用户/排障时一眼能对上。
# 表由产物本身推导（readelf NEEDED + 构建机解析结果），不是手写清单，因此不会过期。
#
# 用法: gen-components.sh <stage-dir> <variant> [<tarball>]
set -euo pipefail

STAGE="${1:?stage dir}"
VARIANT="${2:?variant}"
[ -d "$STAGE" ] || { echo "no such stage: $STAGE" >&2; exit 2; }

OUT="$STAGE/COMPONENTS.md"
BIN="$STAGE/vsr-player"
LIB="$STAGE/lib/vsr-player"
VER="$(grep -oP "version: '\\K[^']+" "$(dirname "$0")/../meson.build" 2>/dev/null | head -1)"

needed_of() { readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
soname_of() { readelf -d "$1" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p' | head -1; }
resolve() {  # resolve <soname> -> 构建机上解析到的真实路径
    local so="$1" p
    p="$(ldd "$BIN" 2>/dev/null | awk -v n="$so" '$1==n{print $3; exit}')"
    [ -n "$p" ] && [ -e "$p" ] && { printf '%s' "$p"; return; }
    ldd "$LIB/libmpv.so.2" 2>/dev/null | awk -v n="$so" '$1==n{print $3; exit}'
}
human() { numfmt --to=iec --suffix=B "$1" 2>/dev/null || echo "$1 B"; }

# 本包自带的库（除 VFX 之外的一切 .so）
mapfile -t BUNDLED < <(find "$LIB" -maxdepth 1 -name '*.so*' -type f | sort)

# 所有 NEEDED（去掉 glibc 家族：那不是"组件"，是操作系统底座）
mapfile -t NEEDED < <( { needed_of "$BIN"; needed_of "$LIB/libmpv.so.2"; } \
    | grep -vE '^(libc|libm|libpthread|libdl|librt|libgcc_s|libmvec|libresolv|libutil|libnsl)\.so' \
    | sort -u)

is_bundled() { local n="$1" f; for f in "${BUNDLED[@]}"; do [ "$(basename "$f")" = "$n" ] && { printf '%s' "$f"; return 0; }; done; return 1; }

{
    echo "# 组件版本表 — vsr-player${VER:+ $VER}（$VARIANT）"
    echo
    echo "> 本文件由 \`scripts/gen-components.sh\` 从产物本身推导（readelf NEEDED + 构建机解析结果），"
    echo "> 每次打包自动重新生成，不是手写清单。"
    echo
    echo "- 构建时间：$(date -Iseconds)"
    echo "- 变体：**$VARIANT**"
    if [ "$VARIANT" = full ]; then
        echo "- 自带依赖：**是**（第三方库随包携带，目标机不需要装）"
    else
        echo "- 自带依赖：**否**（第三方库由系统提供；缺失会在安装自检里明确报错，不会静默）"
    fi
    echo "- 构建机：$(uname -srm)"
    for p in /etc/os-release; do
        [ -f "$p" ] && echo "- 构建机发行版：$(. "$p" && echo "${PRETTY_NAME:-?}")"
    done
    echo
    echo "## 本包携带的库（${#BUNDLED[@]} 个）"
    echo
    echo "| 文件 | SONAME | 大小 |"
    echo "|---|---|---|"
    for f in "${BUNDLED[@]}"; do
        printf '| `%s` | %s | %s |\n' "$(basename "$f")" "$(soname_of "$f" || echo "（无 SONAME）")" \
            "$(human "$(stat -c %s "$f")")"
    done
    echo
    if [ "$VARIANT" = full ]; then
        echo "## 目标机额外要求"
    else
        echo "## 要求系统提供（本包未携带；构建机上的版本见右列）"
    fi
    echo
    echo "| SONAME | 构建机版本 | 说明 |"
    echo "|---|---|---|"
    for n in "${NEEDED[@]}"; do
        if bf="$(is_bundled "$n")"; then
            printf '| `%s` | （本包自带） | %s |\n' "$n" "$(basename "$bf")"
            continue
        fi
        p="$(resolve "$n")"
        if [ -n "$p" ] && [ -e "$p" ]; then
            real="$(basename "$(readlink -f "$p")")"
            note="系统提供"
        else
            real="?"
            note="⚠ 构建机上未解析到（请核对）"
        fi
        printf '| `%s` | `%s` | %s |\n' "$n" "$real" "$note"
    done
    echo
    echo "## 外部组件 / 环境要求"
    echo
    echo '| 组件 | 要求 |'
    echo '|---|---|'
    echo '| NVIDIA 驱动 | 支持 VFX SDK 1.3（本机 615.78.08 实测；含 `libcuda.so.1`） |'
    echo '| VideoFX (VFX) SDK | **不随包分发**（NVIDIA SLA）；`install.sh` 首次安装时从 pypi.nvidia.com 取 wheel（当前 0.2.0.0 = SDK 1.3+，含 VideoFrameGeneration / TrueHDR / ngx-vsr 1.8.3） |'
    echo '| Vulkan loader | `libvulkan.so.1`（Qt Quick 渲染必需） |'
    if [ "$VARIANT" = full ]; then
        echo '| Qt | **已随包捆绑**（`lib/libQt6*.so.6` + `lib/qt6/{plugins,qml}`），目标机不需要系统 Qt |'
    else
        echo '| Qt | 系统 Qt **≥ 6.12**（捆绑版见 `--variant=full`） |'
    fi
    echo '| FFmpeg ABI | 见上表各 `libav*/libsw*.so.*`（本包按这些 SONAME 构建） |'
    echo
    echo "## 排障速查"
    echo
    echo '```'
    echo 'vsr-player --help                      # 版本/用法'
    echo 'mpv-vsr --version                      # 内含的 mpv / libplacebo / FFmpeg 运行时版本'
    echo 'readelf -d ~/.local/lib/vsr-player/libmpv.so.2 | grep NEEDED'
    echo 'ldd ~/.local/bin/vsr-player | grep "not found"'
    echo '```'
} > "$OUT"

echo "  ✓ 组件版本表: $OUT（自带 ${#BUNDLED[@]} 个库 / NEEDED ${#NEEDED[@]} 条）"
