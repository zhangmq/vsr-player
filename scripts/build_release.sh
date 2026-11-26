#!/usr/bin/env bash
# build_release.sh — 构建可分发的 vsr-player release tarball。
#
# 用法: ./scripts/build_release.sh [--variant=standard|full]
#                                 [--fruc-backend=vfg|rife] [--engine=<已有引擎>]
#
# 插帧后端（--fruc-backend，默认 vfg）：
#   vfg （默认）：NVIDIA VFX SDK 1.3 VideoFrameGeneration（AI 插帧）。发行包**不含**
#                TensorRT 库（libnvinfer*）也不含 engines/ —— 这是预期布局，不是缺件。
#   rife        ：RIFE + TensorRT 11 引擎；捆绑 libnvinfer(+plugin).so.11，并打入
#                engines/rife_full_fp16.engine（用 --engine= 可跳过 trtexec 复用）。
#                ⚠ 2026-10-18（v0.4.0）起 RIFE 后端冻结，**对外只发布 vfg 包**：
#                rife 这个选项保留给想自建的用户，不再进入 GitHub release。
#
# 产物结构（安装后 $ORIGIN 相对 RPATH 成立）：
#   vsr-player-<ver>[-full]/
#   ├── vsr-player            # GUI 二进制（RUNPATH=$ORIGIN/../lib/vsr-player:$ORIGIN/../lib）
#   ├── mpv-vsr               # CLI 二进制（RPATH 同款）
#   ├── lib/vsr-player/       # 安装 → ~/.local/lib/vsr-player/（与 VFX 汇合）
#   │   ├── libmpv.so.2       #    libmpv + 闭包捆绑依赖（RPATH 同款）
#   │   ├── deps.manifest     #    完整系统依赖清单（standard 的"不符合就不能用"契约）
#   │   ├── 光盘媒体栈         #    libcdio*/libdvd*/libbluray（cdda/蓝光/ISO）· 仅 full
#   │   ├── libplacebo + shader 栈 + libmujs + libsndio  # 仅 full（SONAME 易变，见 4b2/4b3）
#   │   ├── ffmpeg ×7         #    仅 full（standard 用系统 FFmpeg，NEEDED 去版本化）
#   │   ├── libnvrtc/builtins/cudart.so.13   # 仅 full（standard 用系统 CUDA runtime）
#   │   └── libnvinfer(+plugin).so.11        # 仅 rife：TRT 11.2.1.2（engine 版本绑定）
#   ├── lib/                  # 仅 full：Qt6 库（插件 RUNPATH 期望 <prefix>/lib）
#   ├── lib/qt6/plugins/      # 仅 full：platforms(wayland/xcb) + wayland-* + imageformats
#   ├── lib/qt6/qml/          # 仅 full：QtQuick/QtQuick.Controls/Dialogs/Layouts/Shapes/QtQml
#   ├── engines/rife_full_fp16.engine  # 仅 rife：ampere+ 跨架构（30/40/50 系通用）
#   ├── fonts/  translations/  licenses/  data/  README.md
#   └── install.sh            # 用户端安装（见 scripts/install.sh）
#
# 流程：mpv dist 构建 → client dist 构建 → 依赖闭包收集 → [rife: engine] → 资产 → tarball
# 依赖：meson/ninja/lrelease（Qt6 翻译）/Qt6（full 变体）；
#       rife 后端另需 trtexec（系统 TensorRT）现建引擎，或用 --engine= 复用已构建引擎。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# ── 变体 + 插帧后端 ──────────────────────────────────────────────────
# standard（默认）：薄包。只带 libmpv + 两个修补过的 Qt 库 + 两个 Wayland 模块；
#                   **所有系统级依赖一律由系统提供**（FFmpeg/CUDA/Qt/光盘栈/libplacebo/
#                   libsndio…），并把完整依赖清单写进 lib/vsr-player/deps.manifest。
#                   契约：清单里任一项在目标系统解析不到 → install.sh 明确失败
#                   （"不符合就不能用"），不做静默降级。
# full：额外捆绑 FFmpeg ×7 + CUDA runtime（libnvrtc/builtins/cudart）+ 最小 Qt
#       + 光盘媒体栈 + libplacebo（含 shader 栈）+ libmujs + libsndio ——
#       即"已知会因发行版升级而坏"的那批；驱动/Vulkan/音频会话/X11/Wayland
#       这些系统级 ABI 仍由系统提供。VFX SDK 两个变体都**不捆**（NVIDIA 许可禁止）。
# 依赖收集 = 传递闭包 + 系统白名单：非白名单的 NEEDED 自动捆绑，
# 因此系统 soname 升级（如 libcdio .so.19→.so.21）不会漏收。
VARIANT=standard
# 插帧后端（见头部说明）：vfg = VFX VideoFrameGeneration（默认，不带 TRT/engine）；
# rife = RIFE + TensorRT 引擎（保持既有行为）。
FRUC_BACKEND=vfg
ENGINE_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --variant=standard) VARIANT=standard ;;
        --variant=full)     VARIANT=full ;;
        --variant=*) echo "❌ 未知变体: ${1#--variant=}（支持 standard | full）" >&2; exit 2 ;;
        --fruc-backend=vfg)  FRUC_BACKEND=vfg ;;
        --fruc-backend=rife) FRUC_BACKEND=rife ;;
        --fruc-backend)      FRUC_BACKEND="${2:?--fruc-backend 需要参数}"; shift
                             case "$FRUC_BACKEND" in
                                 vfg|rife) ;;
                                 *) echo "❌ 未知插帧后端: $FRUC_BACKEND（支持 vfg | rife）" >&2; exit 2 ;;
                             esac ;;
        --fruc-backend=*)   echo "❌ 未知插帧后端: ${1#--fruc-backend=}（支持 vfg | rife）" >&2; exit 2 ;;
        --engine=*)         ENGINE_OVERRIDE="${1#--engine=}" ;;
        --engine)           ENGINE_OVERRIDE="${2:?--engine 需要路径}"; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "❌ 未知参数: $1（用法: $0 [--variant=standard|full] [--fruc-backend=vfg|rife] [--engine=<已有引擎>]）" >&2; exit 2 ;;
    esac
    shift
done

VERSION="$(grep -oP "version: '\K[^']+" meson.build | head -1)"
# 双路径 RPATH：lib/vsr-player（自有捆绑库）+ lib（full 变体的 Qt 库）
# NOTE: `$ORIGIN` itself must be part of the rpath. `libmpv.so.2` lives in
# `<prefix>/lib/vsr-player/` while `mpv-vsr` lives in `<prefix>/bin/`, so the two
# have different $ORIGINs. With only `$ORIGIN/../lib/vsr-player` the libmpv case
# resolved to the non-existent `<prefix>/lib/lib/vsr-player`, so every NEEDED
# FFmpeg library silently fell back to the system ones (measured 2026-11-20: the
# full package bundled FFmpeg but never used it, and could not start at all on a
# machine without system FFmpeg 63).
DIST_RPATH='$ORIGIN:$ORIGIN/../lib/vsr-player:$ORIGIN/../lib'
MPV_DIST="$PROJECT_ROOT/build/mpv-dist"
CLIENT_DIST="$PROJECT_ROOT/build/client-dist"
# 产物名带后端后缀（仅当后端 ≠ 默认 vfg）：vfg 与 rife 两个包必须能同时存在，
# 否则同名互相覆盖（本仓库同版本要同时产出两种后端的包）。
case "$FRUC_BACKEND" in
    vfg)  BACKEND_SUFFIX="" ;;
    *)    BACKEND_SUFFIX="-$FRUC_BACKEND" ;;
esac
if [ "$VARIANT" = full ]; then
    STAGE="$PROJECT_ROOT/build/release-staging-full"
    TARBALL="$PROJECT_ROOT/build/vsr-player-$VERSION-linux-x86_64-full$BACKEND_SUFFIX.tar.xz"
    PKG_NAME="vsr-player-$VERSION-full$BACKEND_SUFFIX"
else
    STAGE="$PROJECT_ROOT/build/release-staging"
    TARBALL="$PROJECT_ROOT/build/vsr-player-$VERSION-linux-x86_64$BACKEND_SUFFIX.tar.xz"
    PKG_NAME="vsr-player-$VERSION$BACKEND_SUFFIX"
fi
STAGE_BASE="$(basename "$STAGE")"

echo "=== vsr-player release build (v$VERSION, variant=$VARIANT, fruc_backend=$FRUC_BACKEND) ==="

# ── 1. mpv dist 构建（独立树 + release + dist-rpath）──────────────────
echo "--- [1/6] mpv (dist build, fruc_backend=$FRUC_BACKEND) ---"
MPV_BUILD_DIR="$MPV_DIST" DIST_RPATH="$DIST_RPATH" BUILDTYPE=release \
    FRUC_BACKEND="$FRUC_BACKEND" \
    ./scripts/build_mpv.sh

# ── 2. client dist 构建 ──────────────────────────────────────────────
echo "--- [2/6] client (dist build, fruc_backend=$FRUC_BACKEND) ---"
rm -rf "$CLIENT_DIST"
meson setup "$CLIENT_DIST" --buildtype=release \
    -Ddist-rpath="$DIST_RPATH" \
    -Dmpv-build-dir="$MPV_DIST/_build" \
    -Dfruc_backend="$FRUC_BACKEND" >/dev/null
ninja -C "$CLIENT_DIST"

# ── 3. 组装目录 ─────────────────────────────────────────────────────
echo "--- [3/6] staging ---"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/vsr-player" "$STAGE/fonts" \
         "$STAGE/translations" "$STAGE/licenses"
# engines/ 只属于 rife 后端；vfg（默认）tarball 内**不含**该目录。
if [ "$FRUC_BACKEND" = rife ]; then
    mkdir -p "$STAGE/engines"
fi
cp "$CLIENT_DIST/src/client/vsr-player" "$STAGE/vsr-player"
cp "$MPV_DIST/_build/mpv" "$STAGE/mpv-vsr"
cp -L "$MPV_DIST/_build/libmpv.so.2" "$STAGE/lib/vsr-player/libmpv.so.2"

# ── Qt 6.12.0 fixes (QTBUG-151831 fullscreen freeze, QTBUG-150279 close deadlock) ──
# The patched libQt6Quick / libQt6Gui are built from 6.12.0 sources, so they only make
# sense for a host Qt that is exactly 6.12.0: anything older is ABI-incompatible, and
# 6.12.1+ already carries both upstream fixes (shipping ours would shadow a newer
# official library and fail at load time with "undefined symbol:
# _ZN14QObjectPrivateC2E16QtPrivate_6_12_0, version Qt_6_PRIVATE_API"). install.sh
# applies the same host-version gate when it decides what to place in ~/.local.
sys_qt_version() {
    local v=""
    if command -v qmake6 >/dev/null 2>&1; then
        v="$(qmake6 -query QT_VERSION 2>/dev/null || true)"
    fi
    if [ -z "$v" ]; then
        local f
        for f in /usr/lib/libQt6Core.so.6.* /usr/lib64/libQt6Core.so.6.* \
                 /usr/lib/x86_64-linux-gnu/libQt6Core.so.6.*; do
            [ -e "$f" ] || continue
            v="${f##*.so.}"
            break
        done
    fi
    printf '%s' "$v"
}
QT_FIX_LIB="${QT_QUICK_FIX_LIB:-$PROJECT_ROOT/build/qt-fix/libQt6Quick.so.6.12.0}"
QT_GUI_FIX_LIB="${QT_GUI_FIX_LIB:-$PROJECT_ROOT/build/qt-fix/lib/libQt6Gui.so.6}"
SYS_QT="$(sys_qt_version)"
if [ -n "$SYS_QT" ] && [ "$(printf '%s\n6.12.1\n' "$SYS_QT" | sort -V | head -1)" = "6.12.1" ]; then
    echo "  skip Qt fix libraries (host Qt $SYS_QT already contains QTBUG-151831/150279)"
elif [ -f "$QT_FIX_LIB" ]; then
    cp -aL "$QT_FIX_LIB" "$STAGE/lib/vsr-player/libQt6Quick.so.6"
    echo "  ok Qt Quick fix library (QTBUG-151831) -> lib/vsr-player/libQt6Quick.so.6"
    if [ -f "$QT_GUI_FIX_LIB" ]; then
        cp -aL "$QT_GUI_FIX_LIB" "$STAGE/lib/vsr-player/libQt6Gui.so.6"
        echo "  ok Qt Gui fix library (QTBUG-150279) -> lib/vsr-player/libQt6Gui.so.6"
    else
        echo "  ERROR: missing Qt Gui fix library: $QT_GUI_FIX_LIB (close deadlock, QTBUG-150279)" >&2
        echo "     Build instructions: build/qt-fix/README.md; set VSR_SKIP_QT_FIX=1 to opt out explicitly." >&2
        exit 1
    fi
elif [ "${VSR_SKIP_QT_FIX:-0}" = "1" ]; then
    echo "  WARNING: VSR_SKIP_QT_FIX=1 -- Qt fix libraries omitted; this package still freezes on fullscreen with Qt 6.12.0"
else
    echo "  ERROR: missing Qt Quick fix library: $QT_FIX_LIB (host Qt ${SYS_QT:-unknown})" >&2
    echo "     Build instructions: build/qt-fix/README.md; set VSR_SKIP_QT_FIX=1 to opt out explicitly." >&2
    exit 1
fi

# ── 4. 依赖库收集（显式核心集 + 媒体栈动态发现 + full 变体 Qt）────────
echo "--- [4/6] bundled libs ---"
BIN_REF="$MPV_DIST/_build/mpv"
needed_of() { readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

collect_to() {  # collect_to <outdir> <soname> <search-dirs...>（SONAME 即目标文件名）
    local out="$1" soname="$2"; shift 2
    local found=""
    for d in "$@"; do
        [ -n "$d" ] || continue
        # -L：跟随目录软链（/opt/cuda/lib64 → targets/x86_64-linux/lib）；
        # 软链文件也可（cp -L 解引用）；名称精确或带 minor 后缀（如 .13.3）
        found="$(find -L "$d" -maxdepth 1 \( -name "$soname" -o -name "$soname.*" \) 2>/dev/null | head -1)"
        [ -n "$found" ] && break
    done
    if [ -z "$found" ]; then
        found="$(ldd "$BIN_REF" | awk -v p="$soname" '$1==p {print $3}' | head -1)"
    fi
    if [ -z "$found" ] || [ ! -e "$found" ]; then
        echo "  ❌ $soname not found" >&2; return 1
    fi
    # 复制为 SONAME 文件名（解引用软链——单一文件即可满足加载器按名查找）
    cp -L "$found" "$out/$soname"
    echo "  ✓ $soname ($(basename "$found"))"
}
LIB="$STAGE/lib/vsr-player"
collect() { collect_to "$LIB" "$@"; }

# 4a. 核心集：FFmpeg / CUDA runtime 仅 full 变体捆绑（standard 用系统提供——分层是
#     用户 2026-10-26 定的：standard = 薄包，full = 自洽包）。
#     rife 后端才有的 TensorRT 保持原样（版本绑定，显式固定）。
if [ "$VARIANT" = full ]; then
# FFmpeg 的 soname **从 libmpv 的 NEEDED 推导**，不硬编码版本号：以后 ffmpeg
# 大版本升级（63→64）自动跟随，脚本不会打出一个钉死旧 soname 的包。
for f in $(needed_of "$LIB/libmpv.so.2" | grep -E '^lib(av|sw)' | sort -u); do
    collect "$f" /usr/lib
done
# CUDA runtime：**不要钉 minor/patch 版本号**，从 /opt/cuda/lib64 发现实际 soname。
# 2026-10 事故（issue #14）：Toolkit 13.3→13.4 升级后，钉死的
# `libnvrtc-builtins.so.13.3` 让发布构建在 [4/6] 直接 "not found" 中止，
# 报错还容易被误判成 CUDA 装坏了。
# 发现方式：解析无版本软链（libX.so → libX.so.<N>.<M>.<P>）再读它的 ELF SONAME
# —— SONAME 才是加载器要找的名字。注意 libnvrtc-builtins 并不出现在任何 NEEDED
# 里（libnvrtc 运行时按内部版本取用，实测 readelf 无该条目），因此只能这样发现，
# 不能像下面的媒体栈那样从 NEEDED 推导。
cuda_collect() {  # cuda_collect <基础名>（libnvrtc / libnvrtc-builtins / libcudart）
    local base="$1" target="" soname=""
    target="$(readlink -f "/opt/cuda/lib64/$base.so" 2>/dev/null || true)"
    if [ -n "$target" ] && [ -e "$target" ]; then
        soname="$(readelf -d "$target" 2>/dev/null \
                  | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p' | head -1)"
    fi
    if [ -z "$soname" ]; then
        echo "  ❌ CUDA runtime 缺 $base：/opt/cuda/lib64 下找不到可解析的 $base.so" >&2
        echo "     （CUDA Toolkit 未安装或不在 /opt/cuda —— 属环境问题，非本仓库缺陷）" >&2
        return 1
    fi
    collect "$soname" /opt/cuda/lib64
}
cuda_collect libnvrtc
cuda_collect libnvrtc-builtins
cuda_collect libcudart
else
    # standard 的系统依赖同样从 NEEDED 推导，构建日志里直接列出"这包要求系统有什么"，
# 而不是写死一份可能过期的清单。
    SYS_DEPS="$(needed_of "$LIB/libmpv.so.2" \
                | grep -E '^lib(av|sw|nvrtc|cudart)' | sort -u | tr '\n' ' ')"
    echo "  · standard：FFmpeg / CUDA 用系统提供 —— 要求系统提供: ${SYS_DEPS:-（无）}"
fi
# TensorRT 库只有 rife 后端需要（引擎反序列化）；vfg 包不收集 libnvinfer*。
if [ "$FRUC_BACKEND" = rife ]; then
    collect libnvinfer.so.11 /usr/lib
    collect libnvinfer_plugin.so.11 /usr/lib || echo "  ⚠ plugin missing（RIFE 图无 plugin 算子，可缺）"
fi

# 4b. Optical/Blu-ray media stack: discover the sonames from the actual
# NEEDED entries instead of hard-coding them. full only: the standard package
# is a thin package by contract (system libraries + a dependency manifest that
# install.sh enforces), while full carries everything that can break on a
# distro upgrade. This group changed SONAME twice in 2026-09 (libcdio
# .so.19->.so.21, libbluray .so.3->.so.4).
if [ "$VARIANT" = full ]; then
MEDIA=""
prev=""
for _ in 1 2 3 4; do
    MEDIA="$(for f in "$LIB"/*.so*; do
                 [ -f "$f" ] || continue
                 needed_of "$f" | grep -E '^lib(cdio|dvd|bluray)' || true
             done | sort -u)"
    [ "$MEDIA" = "$prev" ] && break
    for f in $MEDIA; do [ -e "$LIB/$f" ] || collect "$f" /usr/lib; done
    prev="$MEDIA"
done
[ -n "$MEDIA" ] && echo "  ✓ 媒体栈: $(printf '%s' "$MEDIA" | tr '\n' ' ')"
else
    echo "  · standard：光盘媒体栈由系统提供（缺了由 install.sh 的依赖清单校验拦下）"
fi

# 4b2. Libraries that cannot be substituted across versions: full only (the
# standard package keeps them as system dependencies and refuses to start on a
# mismatch instead of gambling on the ABI).
#  1) libplacebo: its SONAME is the API version and jumps on every libplacebo
#     release (7.360 -> 7.387 in 2026-11 broke every installed package). mpv
#     passes api_ver to pl_log_create, which libplacebo treats as "historical,
#     ignored currently" (/usr/include/libplacebo/log.h:69), so linking across
#     versions is a silent ABI gamble rather than a clean error.
#  2) libmujs: it has NO SONAME, so the link records the absolute path
#     (/usr/lib/libmujs.so) in NEEDED (Debian: /usr/lib/x86_64-linux-gnu/),
#     which breaks on any other distro layout. rewrite-needed.py rewrites that
#     entry to a basename, so the library itself has to be carried.
PLACEBO_SO="$(needed_of "$LIB/libmpv.so.2" | grep -E '^libplacebo\.so\.[0-9]+$' | head -1)"
if [ "$VARIANT" = full ] && [ -n "$PLACEBO_SO" ]; then
    collect "$PLACEBO_SO" /usr/lib
    # libplacebo's shader compiler stack (glslang/shaderc) and the Dolby Vision
    # helper, derived from NEEDED and iterated to a fixed point (glslang pulls
    # in the libSPIRV-Tools(-opt) layer).
    for _ in 1 2 3 4 5; do
        added=0
        for f in $(for b in "$LIB"/*.so*; do
                       needed_of "$b" | grep -E '^lib(shaderc|glslang|SPIRV|dovi)'
                   done | sort -u); do
            [ -e "$LIB/$f" ] && continue
            collect "$f" /usr/lib >/dev/null && added=$((added + 1))
            echo "  ✓ $f（libplacebo shader 栈）"
        done
        [ "$added" -eq 0 ] && break
    done
    collect libmujs.so /usr/lib
    echo "  ✓ 小库（跨版本不可替换）: $PLACEBO_SO（含 shader 栈）+ libmujs.so"
elif [ "$VARIANT" = full ]; then
    echo "  ⚠ libmpv 未链接 libplacebo？（预期必有）" >&2
else
    echo "  · standard：libplacebo / libmujs / 完整依赖清单见 lib/vsr-player/deps.manifest"
fi

# 4b3. libsndio: pulled in by mpv's sndio AO. Its SONAME went .so.7 -> .so.9
# on 2026-11-21 and made every installed package fail at the dynamic loader.
# full carries it; standard declares it in deps.manifest and refuses to start
# when the system no longer provides it.
SNDIO_SO="$(needed_of "$LIB/libmpv.so.2" | grep -E '^libsndio\.so\.[0-9]+$' | head -1)"
if [ "$VARIANT" = full ] && [ -n "$SNDIO_SO" ]; then
    collect "$SNDIO_SO" /usr/lib >/dev/null
    echo "  ✓ $SNDIO_SO（音频后端，SONAME 易变）"
fi

# 4c. full 变体：最小 Qt（镜像发行版布局，插件自带相对 RUNPATH 才能解析）
#     布局：<prefix>/lib/{libQt6*.so.6} + <prefix>/lib/qt6/{plugins,qml}
if [ "$VARIANT" = full ]; then
    mkdir -p "$STAGE/lib/qt6/plugins/platforms" "$STAGE/lib/qt6/qml"
    # 平台插件只取需要的三个：wayland（主）/ xcb（X11 回退）/ offscreen（无头自检）。
    # 整目录会拖入 eglfs/linuxfb/vnc（→ libmtdev/libts/libinput 等无关依赖）。
    for p in libqwayland.so libqxcb.so libqoffscreen.so; do
        [ -f "/usr/lib/qt6/plugins/platforms/$p" ] && \
            cp -a "/usr/lib/qt6/plugins/platforms/$p" "$STAGE/lib/qt6/plugins/platforms/"
    done
    # Wayland 集成插件（QtWaylandClient 的 shell/decoration/graphics 后端）
    for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
        [ -d "/usr/lib/qt6/plugins/$d" ] && cp -a "/usr/lib/qt6/plugins/$d" "$STAGE/lib/qt6/plugins/"
    done
    # 不拷 imageformats：本项目 QML 不加载图片；Arch 该目录含 KDE kimg_* 插件，
    # 会拖入 libraw/libheif/libavif/libKF6Archive 等无关依赖。
    # QML 模块：QML import 清单（QtQuick / Controls / Dialogs / Layouts / Shapes / QtQml）
    for m in QtQuick QtQml; do
        [ -d "/usr/lib/qt6/qml/$m" ] && cp -a "/usr/lib/qt6/qml/$m" "$STAGE/lib/qt6/qml/"
    done
    # Drop QtQuick/Pdf and QtQuick/VirtualKeyboard: those QML plugins pull
    # libQt6Pdf* / libQt6VirtualKeyboard* into the dependency closure, and Qt
    # VirtualKeyboard is GPLv3-only (no LGPL option) while this project does not use
    # it at all (the QML only imports QtQuick/QtQml). Qt PDF does offer LGPLv3 but is
    # equally unused, so it is removed as well to save space.
    rm -rf "$STAGE/lib/qt6/qml/QtQuick/Pdf" "$STAGE/lib/qt6/qml/QtQuick/VirtualKeyboard"
    # Qt 库：直接链接的 + 运行时才加载的（插件/QML 模块 NEEDED，迭代补齐）
    for f in $(needed_of "$STAGE/vsr-player" | grep '^libQt6' || true); do
        collect_to "$STAGE/lib" "$f" /usr/lib
    done
    for _ in 1 2 3 4; do
        added=0
        while read -r dep; do
            [ -n "$dep" ] || continue
            [ -e "$STAGE/lib/$dep" ] && continue
            collect_to "$STAGE/lib" "$dep" /usr/lib >/dev/null && added=$((added+1))
        done < <(find "$STAGE/lib" -name '*.so*' -type f -exec readelf -d {} \; 2>/dev/null \
                 | sed -n 's/.*(NEEDED).*\[\(lib\(Qt6\|LayerShellQt\)[^]]*\)\]/\1/p' | sort -u)
        [ "$added" -eq 0 ] && break
    done
    echo "  ✓ Qt: $(find "$STAGE/lib" -maxdepth 1 -name 'libQt6*' | wc -l) 个库, $(find "$STAGE/lib/qt6/plugins" -name '*.so' | wc -l) 个插件, $(find "$STAGE/lib/qt6/qml" -name '*.so' | wc -l) 个 QML 模块"
fi

# 4d. 闭包自检（D-2）：每个 NEEDED 必须"已捆绑"或属系统白名单，否则打包失败。
#     白名单 = 由系统/驱动提供：glibc 家族、驱动、Vulkan/图形/输入、字体栈、
#     音频后端、系统服务、压缩/加密基础库，以及 FFmpeg 的可选编解码依赖。
ALLOW='ld-linux|libc\.so|libm\.so|libpthread|libdl\.so|librt\.so|libgcc_s|libmvec|libresolv|libutil|libnsl'
ALLOW+='|libcuda|libnvidia|libvulkan|libX|libxcb|libwayland|libxkbcommon|libEGL|libGL|libGLX|libgbm|libdrm|libdisplay-info|libpciaccess|libv4l'
ALLOW+='|libasound|libpulse|libpipewire|libjack|libsndio|libopenal|libSDL|libdecor'
ALLOW+='|libdbus|libsystemd|libselinux|libudev|libcap|libacl|libattr|libproxy|libpxbackend'
ALLOW+='|libfontconfig|libfreetype|libharfbuzz|libicu|libxml2|libexpat|libz\.so|libbz2|libpng|libbrotli|libgraphite|libfribidi|libthai|libdatrie|libsharpyuv'
ALLOW+='|libpcre|libffi|libmount|libblkid|liblzma|libzstd|liblz4|libdeflate|libgcrypt|libgpg-error|libgnutls|libnettle|libhogweed|libidn2|libunistring|libtasn1|libp11-kit'
ALLOW+='|librsvg|libcairo|libpixman|libuuid|libcrypt|libkeyutils|libcom_err|libkrb5|libgssapi|libtirpc|libbsd|libmd|libsodium|libpgm|libnorm|libzmq'
ALLOW+='|libstdc\+\+|libgomp|libnuma|libtbb|libhwloc|libelf|libdw|libunwind|libgmp|libmpfr|libmpc'
ALLOW+='|libglib|libgthread|libgio|libgobject|libgmodule|libncurses|libtinfo|libreadline|libgdbm|libdb-|libsqlite|libjson|libyaml|libb2'
ALLOW+='|libssl|libcrypto|libcurl|libnghttp|libpsl|libssh|libldap|liblber|libsasl|libcares'
ALLOW+='|libx264|libx265|libvpx|libdav1d|libaom|libSvt|libsvt|librav1e|libxvid|libkvazaar|libfdk|libopencore|libvo-|libcodec2|libgsm|libilbc|libopenh264|libshine|libsnappy|libspeex|libunibreak|libbs2b|libmysofa|libserd|libsord|libsamplerate|libzix|liblrdf|libcjson|libvmaf|libvidstab|libmfx|libvpl|libzvbi|libaribb24|libaribcaption'
ALLOW+='|libass|libplacebo|libuchardet|libjpeg|libjxl|libhwy|libluajit|liblua|libarchive|libopus|libvorbis|libogg|libflac|libmp3lame|libtwolame|libwavpack|libtheora|libsixel|libzimg|libva|libvdpau|libopenmpt|libmodplug|libgme|libchromaprint|librabbitmq|librubberband|libsoxr|libsrt|libsmbclient|librist|libsrtp|libmicrodns'
ALLOW+='|libOpenCL|libclang|libLLVM|libwayland-egl'
# full 变体下 Qt 自身的 C 依赖（发行版随 Qt 提供；捆绑 Qt 但复用系统 C 栈）
ALLOW+='|libOpenGL|libGLdispatch|libGLESv2|liburing|libdouble-conversion|libmd4c|libb2|libSM|libICE|libmtdev|libts|libinput|libhunspell'
# mpv/FFmpeg 的可选特性依赖（输出/图像/采集），发行版常规组件
ALLOW+='|libmujs|liblcms2|libcaca|libwebp|libopenjp2|libraw1394|libavc1394|librom1394|libiec61883|libbs2b|libflite|libOpenEXR|libIlmThread|libImath|libzvbi|libdc1394'
# standard 变体：Qt / FFmpeg / CUDA runtime 全由系统提供；full 变体则必须已捆绑
if [ "$VARIANT" = standard ]; then
    ALLOW+='|libQt6'
    ALLOW+='|libavcodec|libavformat|libavutil|libavfilter|libavdevice|libswscale|libswresample'
    ALLOW+='|libnvrtc|libcudart'
fi

validate_closure() {
    local bad=0 f dep base
    while read -r f; do
        while read -r dep; do
            [ -n "$dep" ] || continue
            base="$(basename "$dep")"          # 绝对路径依赖（无 SONAME 的库）按 basename 判定
            if [ -e "$LIB/$base" ] || [ -e "$STAGE/lib/$base" ]; then continue; fi
            printf '%s' "$base" | grep -qE "^($ALLOW)" && continue
            # standard 变体按设计**不携带第三方库**（用户 2026-10-26 的发版规则）：
            # 未捆绑的依赖只要构建机能解析（ldconfig 缓存里有），就算"系统提供"，
            # 会写进 COMPONENTS.md 的"要求系统提供"表。真正的错误只有一种：
            # 连构建机都解析不到 —— 那说明 SONAME 拼错或依赖缺失。
            if [ "$VARIANT" = standard ] && \
               ldconfig -p 2>/dev/null | awk -v n="$base" '$1==n{found=1} END{exit !found}'; then
                continue
            fi
            echo "  ❌ $(basename "$f") → $dep（未捆绑、不在白名单，且构建机也解析不到）"
            bad=1
        done < <(needed_of "$f")
    done < <(find "$STAGE" -type f \( -name '*.so*' -o -perm -u+x \) 2>/dev/null)
    return "$bad"
}
if validate_closure; then
    echo "  ✓ 闭包自检通过（无未捆绑且非白名单的依赖）"
else
    echo "❌ 闭包自检失败：上列依赖需捆绑或加入白名单（build_release.sh 的 ALLOW）" >&2
    exit 1
fi

# ── 5. engine + 资产 ────────────────────────────────────────────────
# engine 段落只属于 rife 后端；vfg（默认）整段跳过 → tarball 无 engines/。
if [ "$FRUC_BACKEND" = rife ]; then
    echo "--- [5/6] engine + assets ---"
    if [ -n "$ENGINE_OVERRIDE" ]; then
        [ -f "$ENGINE_OVERRIDE" ] || { echo "❌ --engine 指定的文件不存在: $ENGINE_OVERRIDE" >&2; exit 1; }
        cp "$ENGINE_OVERRIDE" "$STAGE/engines/rife_full_fp16.engine"
        echo "  ✓ 复用引擎（跳过 trtexec）: $ENGINE_OVERRIDE"
        echo "  ⚠ 复用引擎必须与目标 TRT 版本一致（引擎内嵌构建时的 TRT 版本，不匹配会被拒绝反序列化）"
    else
        command -v trtexec >/dev/null 2>&1 || {
            echo "❌ --fruc-backend=rife 未给 --engine 且系统找不到 trtexec（RIFE 引擎需 trtexec 现建）" >&2
            echo "   任选其一：--engine=<已构建引擎> 复用，或安装 TensorRT（含 trtexec）后重跑" >&2
            exit 1
        }
        bash tests/fruc/build_rife_full_engine.sh --hardware-compat=on \
            "$STAGE/engines/rife_full_fp16.engine"
    fi
else
    echo "--- [5/6] assets (fruc_backend=vfg：无 RIFE engine) ---"
    [ -z "$ENGINE_OVERRIDE" ] || echo "  ⚠ 已指定 --engine 但 fruc_backend=vfg：忽略（VFG 包不含 engines/）"
fi
cp third_party/material-icons/materialdesignicons-webfont.ttf "$STAGE/fonts/"
# 桌面集成（install.sh 用它写 ~/.local/share/applications/：模板 .desktop.in
# + 图标 svg/png）——放包根的 data/，install.sh 按 $SRC/data/ 找。
rm -rf "$STAGE/data"; cp -r "$PROJECT_ROOT/data" "$STAGE/data"
cp "$CLIENT_DIST/src/client/"*.qm "$STAGE/translations/"

# ── 6. 许可 + README + tarball ──────────────────────────────────────
echo "--- [6/6] licenses + tarball ---"
# NVIDIA 许可文本（随软件分发的许可要求）；来源：nvidia-vfx wheel 内附
#（PyPI 官方包）。本地 pip 缓存缺失时跳过（README 引用官方 URL）。
WHEEL="$(find "$HOME/.cache/pip" -name "nvidia_vfx*.whl" 2>/dev/null | head -1)"
if [ -n "$WHEEL" ]; then
    # 流式提取许可文件（unzip -l 列名 + -p 提取；不解压 wheel 本体）
    mkdir -p "$STAGE/licenses"
    unzip -l "$WHEEL" | awk '/licenses\/packaging\/.*\.(pdf|md|txt)$/ {print $4}' | while read -r f; do
        [ -n "$f" ] || continue
        unzip -p "$WHEEL" "$f" > "$STAGE/licenses/$(basename "$f")"
    done
    if [ "$(ls "$STAGE/licenses" | wc -l)" -gt 0 ]; then
        echo "  ✓ NVIDIA SLA (from nvidia-vfx wheel)"
    else
        rmdir "$STAGE/licenses"
        echo "  ⚠ wheel 中未找到许可文件"
    fi
else
    echo "  ⚠ nvidia-vfx wheel not in pip cache — licenses/ 缺 SLA 文本（README 有链接）"
fi

# Qt runtime licenses plus the "libQt6Quick/libQt6Gui in this package are modified"
# notice (LGPL compliance: ship the license texts, state the modification and tell
# users how to obtain the corresponding source -- patches in docs/patches/, rebuild
# steps in build/qt-fix/). Runs unconditionally: any package that ships a Qt library
# (standard: the fix libraries; full: the whole Qt) must include it.
if [ -f "$PROJECT_ROOT/licenses/LGPL-3.0-only.txt" ]; then
    mkdir -p "$STAGE/licenses"
    # LGPLv3 section 4(b): both the GPL-3.0 and the LGPL-3.0 texts are required
    # (LGPLv3 is an additional permission on top of GPLv3).
    cp "$PROJECT_ROOT/licenses/LGPL-3.0-only.txt" "$STAGE/licenses/"
    cp "$PROJECT_ROOT/licenses/GPL-3.0-only.txt" "$STAGE/licenses/" 2>/dev/null || true
    cp "$PROJECT_ROOT/licenses/QT-MODIFICATION-NOTICE.md" "$STAGE/licenses/" 2>/dev/null || true
    echo "  ok Qt licenses (LGPL-3.0 + GPL-3.0) + modification notice -> licenses/"
fi
cat > "$STAGE/README.md" << 'EOR'
# vsr-player vVERSION_PLACEHOLDER

NVIDIA GPU 实时 AI 视频处理播放器：**超分 + 降噪**、**插帧**（FRUC_TITLE_PLACEHOLDER）与
**HDR**（10-bit RGB10A2 全链、HDR10 上屏、SDR→HDR10 上变换）。libmpv + Qt6 + NVIDIA VFX SDK。

## 系统依赖（必须）
DEPS_PLACEHOLDER

## 安装
```
./install.sh
```
安装到 ~/.local（bin + lib/vsr-player），不修改任何系统配置。

## VFX SDK（超分引擎，首次安装需下载）
tarball 不含 VFX SDK（NVIDIA SLA 限制再分发）。install.sh 检测缺失时
从 NVIDIA 官方索引 pypi.nvidia.com 选 manylinux wheel 下载并提取（或手动下载后重跑脚本）：
- 官方来源: https://pypi.nvidia.com/nvidia-vfx/（manylinux wheel；pypi.org 只有占位 sdist）
- 许可: NVIDIA Software License Agreement（licenses/，下载前脚本会提示）

FRUC_SECTION_PLACEHOLDER

## HDR
- HDR 片源 × 支持 HDR 的显示 → 自动切 HDR10 上屏（运行期切换、带内容静态元数据；
  SDR 显示下由 mpv tone map 到 SDR）。
- **SDR→HDR10 上变换**（VFX TrueHDR）：画质弹窗里的 `SDR → HDR10` 开关 + `Advanced`
  参数弹窗（目标峰值/亮度、饱和度、对比度、中灰、抗色带），**默认关**。面板非 HDR 也能开：
  此时输出仍为 SDR，由 mpv 把上变换后的 HDR10 帧 tone map 回 SDR。

## 测试现状
项目在有限硬件条件下开发，无法覆盖所有 GPU/驱动/媒体组合——你可能遇到
未发现的问题。遇到问题可让 AI 编码代理协助排查（本项目即 AI 协作开发，
代码与设计记录齐全），也欢迎提交 issue/PR。

## 版本兼容（不匹配时）
- 驱动过旧导致 VSR 加载失败：升级驱动，或锁定旧版 VFX——
  `pip download nvidia-vfx==<版本> --no-deps` 后解压 nvvfx/libs/*.so*
  到 ~/.local/lib/vsr-player/、nvidia_vfx.libs/* 到 ~/.local/nvidia_vfx.libs/
  （0.2.0.0 起的 wheel 把 cudart 放在 nvidia_vfx.libs/，漏掉则 VSR 静默直通）。
  手动放置时需自行补齐无版本软链
  （vsr_proc 以无版本名 dlopen：libnppc.so/libcudnn.so/...，缺软链
  VFX 加载链断裂 → 超分静默直通；install.sh 自动下载路径会补）
FRUC_COMPAT_PLACEHOLDER

## 运行
```
vsr-player [视频或目录]      # GUI
mpv-vsr --vf=vsr:scale=2 <视频>   # CLI（VSR 2x）
```
EOR
sed -i "s/VERSION_PLACEHOLDER/$VERSION/" "$STAGE/README.md"
if [ "$VARIANT" = full ]; then
    DEPS_TEXT='- NVIDIA 显卡 + 驱动（`libcuda.so.1`）
- Vulkan loader（`libvulkan.so.1`）与 Wayland/X11 客户端库（发行版常规组件）
- **Qt6 已随包捆绑**——无需系统 Qt（本变体面向 Qt < 6.12 的发行版）'
else
    DEPS_TEXT='- NVIDIA 显卡 + 驱动（`libcuda.so.1`）
- 系统 Qt ≥ 6.12（GUI 版；`mpv-vsr` 命令行版不需要）
- 系统 FFmpeg（soname 需与本包一致：libavcodec.so.63 等）——本包不捆绑
- 系统 CUDA runtime（libnvrtc.so.13 + libnvrtc-builtins + libcudart.so.13）
  ——本包不捆绑（需 CUDA Toolkit 或同 soname 运行时）'
fi
# README 中随后端分支的段落（vfg 无 TRT/engine，不能留 RIFE 专属说明误导用户）
if [ "$FRUC_BACKEND" = rife ]; then
    FRUC_TITLE='RIFE'
    FRUC_SECTION='## 插帧（RIFE）
engines/rife_full_fp16.engine 为 ampere+ 跨架构构建（Ampere 30 系及以上）。
RTX 20 系（Turing）及更早不支持 → 插帧自动直通（VSR 不受影响）。
插帧需 GPU 支持 FP16 Tensor Cores。'
    FRUC_COMPAT='- RIFE 引擎与 TensorRT 版本绑定（本 tarball 内自洽）；手工重建引擎见
  仓库 tests/fruc/build_rife_full_engine.sh（需 trtexec + ONNX 资产）'
else
    FRUC_TITLE='VFX VideoFrameGeneration'
    FRUC_SECTION='## 插帧（VFX VideoFrameGeneration）
本包为 VFG 后端（NVIDIA VFX SDK 1.3 的 AI 插帧）：**不需要 TensorRT 库，也不含
engines/ 与 RIFE engine——这是预期布局**，不是缺件。插帧随 VFX SDK 运行时提供
（首次安装由 install.sh 下载，见上），需 GPU 支持 FP16 Tensor Cores。'
    FRUC_COMPAT='- 本包为 VFG 后端：无 RIFE 引擎、无 TensorRT 库（因此没有 TRT 版本绑定
  问题）。RIFE 后端（TensorRT）已冻结、**不再提供预编译包**；需要它（Ampere 及更早
  GPU——VFG 需要 Ada/Blackwell）可源码自建：./scripts/build-from-source.sh
  --fruc-backend=rife'
fi
python3 - "$STAGE/README.md" "$DEPS_TEXT" "$FRUC_TITLE" "$FRUC_SECTION" "$FRUC_COMPAT" << 'PYEOF'
import sys
path = sys.argv[1]
repl = {
    'DEPS_PLACEHOLDER': sys.argv[2],
    'FRUC_TITLE_PLACEHOLDER': sys.argv[3],
    'FRUC_SECTION_PLACEHOLDER': sys.argv[4],
    'FRUC_COMPAT_PLACEHOLDER': sys.argv[5],
}
with open(path, encoding='utf-8') as fh:
    content = fh.read()
for key, text in repl.items():
    content = content.replace(key, text)
with open(path, 'w', encoding='utf-8') as fh:
    fh.write(content)
PYEOF
cp scripts/install.sh "$STAGE/install.sh" && chmod +x "$STAGE/install.sh"
# 缺 soname 的兼容兜底（install.sh 的 [6/6] 会在系统 ffmpeg/CUDA 大版本升级后调用它）
cp scripts/soname-compat.sh "$STAGE/soname-compat.sh" && chmod +x "$STAGE/soname-compat.sh"

# ── 依赖清单（standard 的"不符合就不能用"契约）──────────────────────
# 一行一个 soname（不在包内的）：<soname> <needed-by,...> <构建机路径>。
# install.sh 用它把"缺哪个库、谁需要它"讲清楚（否则用户只会看到加载器的
# "cannot open shared object file"）；COMPONENTS.md 是给人读的版本，这里是机器可
# 读的同一份事实。full 变体同样生成，但绝大多数条目会被"包内自带"过滤掉。
gen_deps_manifest() {
    local out="$LIB/deps.manifest" b name dep base son p k
    declare -A need=() path_of=()
    # Build-machine soname -> real path (ldconfig cache), so the manifest records
    # /usr/lib/libfoo.so.N rather than a path relative to the build directory.
    while IFS=$'\t' read -r son p; do
        [ -n "$son" ] && [ -n "$p" ] && path_of["$son"]="$p"
    # Skip /usr/lib32 entries: the manifest documents the 64-bit requirement.
    done < <(ldconfig -p 2>/dev/null | awk '/=>/ && $NF !~ /lib32/{print $1"\t"$NF}')
    for b in "$STAGE/vsr-player" "$STAGE/mpv-vsr" "$LIB/libmpv.so.2"; do
        [ -f "$b" ] || continue
        name="$(basename "$b")"
        while read -r dep; do
            [ -n "$dep" ] || continue
            base="$(basename "$dep")"
            [ -e "$LIB/$base" ] && continue            # bundled
            [ -e "$STAGE/lib/$base" ] && continue      # bundled (Qt layout)
            need["$base"]+=" $name"
        done < <(needed_of "$b")
    done
    {
        echo "# vsr-player deps manifest (variant=$VARIANT, fruc_backend=$FRUC_BACKEND)"
        echo "# format: <soname> <needed-by,...> <build-machine path>"
        echo "# standard: every entry must resolve on the target system, otherwise install.sh fails."
        for son in $(printf '%s\n' "${!need[@]}" | sort); do
            k="$(printf '%s\n' ${need[$son]} | sort -u | tr '\n' ',' | sed 's/,$//')"
            echo "$son $k ${path_of[$son]:-unknown}"
        done
    } > "$out"
    echo "  ✓ 依赖清单: $(grep -vc '^#' "$out") 项 → lib/vsr-player/deps.manifest"
}
gen_deps_manifest

# 组件版本表（发版必备：full 列自带清单，standard 列系统要求）
bash scripts/gen-components.sh "$STAGE" "$VARIANT"

tar -C "$PROJECT_ROOT/build" -cJf "$TARBALL" "$STAGE_BASE" \
    --transform "s/$STAGE_BASE/$PKG_NAME/"
echo ""
echo "=== Done: $TARBALL ($(du -h "$TARBALL" | cut -f1), variant=$VARIANT, fruc_backend=$FRUC_BACKEND) ==="
