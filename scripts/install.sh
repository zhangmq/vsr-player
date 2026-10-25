#!/usr/bin/env bash
# install.sh — 用户端安装 vsr-player（GUI + CLI + 捆绑依赖）。
#
# 环境纯净原则：
#   - 只复制文件到应用自属目录（~/.local/bin + ~/.local/lib/vsr-player/）
#   - 系统依赖（Qt/driver）只检测提示，绝不自动安装
#   - VFX 获取：curl 从 NVIDIA 官方索引（pypi.nvidia.com）wheel 纯下载 + 解压（不调用 pip 安装器，
#     不碰 Python 环境）；**下载/提取失败直接报错退出（exit 1）**，不静默降级
#     （VFX 缺失 = VSR 静默直通，静默成功会让人以为装好了；显式 `--no-vfx` 才允许缺）
#   - PATH 只提示，不修改 shell 配置
#
# 双场景：
#   - tarball 内（分发）：源 = 脚本所在目录（vsr-player + lib/vsr-player/）
#   - 仓库内（dev）：源 = build 树（build/src/client + build/mpv-dist）
#
# 安装目标：
#   ~/.local/bin/vsr-player、mpv-vsr        # RPATH=$ORIGIN/../lib/vsr-player
#   ~/.local/lib/vsr-player/                # 捆绑库 + engine + VFX（共用）
#   ~/.local/share/vsr-player/fonts/        # 图标字体（main.cpp fallback）
#   ~/.local/share/applications/            # 桌面入口（应用菜单/启动器可见）
#   ~/.local/share/icons/hicolor/…/apps/    # 桌面图标（svg + 512 PNG）
#   ~/.local/bin/translations/              # 翻译（main.cpp fallback）
set -euo pipefail

NO_VFX=0
[ "${1:-}" = "--no-vfx" ] && NO_VFX=1

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$HOME/.local/bin"
LIB_DIR="$HOME/.local/lib/vsr-player"
# VFX wheel ≥0.2.0.0 把 libcudart-<hash>.so.* 放在 nvidia_vfx.libs/（nvvfx/ 的兄弟
# 目录）；VFX 库的 RPATH 是 $ORIGIN/../../nvidia_vfx.libs，从 $LIB_DIR 出发唯一能
# 解析的位置就是 <$LIB_DIR 的父目录的父目录>/nvidia_vfx.libs（多处共用，单一事实源）。
VFX_SIBLING="$(dirname "$(dirname "$LIB_DIR")")/nvidia_vfx.libs"
FONT_DIR="$HOME/.local/share/vsr-player/fonts"

# ── 源定位：tarball 场景（旁边有 vsr-player）优先，否则 dev 构建树 ──
SRC="$HERE"
if [ ! -x "$SRC/vsr-player" ] && [ ! -f "$SRC/lib/vsr-player/libmpv.so.2" ]; then
    SRC="$(cd "$HERE/.." && pwd)"   # 仓库根
    MODE=dev
    # dist 构建树（build_release.sh 产物）优先，dev 树（build/）兜底
    if [ -x "$SRC/build/client-dist/src/client/vsr-player" ]; then
        GUI_BIN="$SRC/build/client-dist/src/client/vsr-player"
        CLI_BIN="$SRC/build/mpv-dist/_build/mpv"
        LIB_SRC="$SRC/build/release-staging/lib/vsr-player"
        ENG_SRC="$SRC/build/release-staging/engines"
    elif [ -x "$SRC/build/src/client/vsr-player" ]; then
        GUI_BIN="$SRC/build/src/client/vsr-player"
        CLI_BIN="$SRC/build/mpv/_build/mpv"
        LIB_SRC="$SRC/third_party/nvvfx/lib"     # dev：VFX 目录即库源
        ENG_SRC="$SRC/build/tests/fruc"          # dev：engine 构建目录
        DEV_VFX_COPY=1                            # dev：VFX 一并复制
    else
        echo "❌ 未找到构建产物：既不是 tarball 目录也没有构建树（先跑 ./scripts/build_release.sh 或 dev 构建）" >&2
        exit 1
    fi
else
    MODE=tarball
    GUI_BIN="$SRC/vsr-player"
    CLI_BIN="$SRC/mpv-vsr"
    LIB_SRC="$SRC/lib/vsr-player"
    ENG_SRC="$SRC/engines"
fi
echo "=== vsr-player install (mode=$MODE, src=$SRC) ==="

# ── 1. 系统依赖检查（只检测提示，不安装）────────────────────────────
echo "--- [1/6] system check ---"
OK=1
if ldconfig -p 2>/dev/null | grep -q "libcuda\.so\.1" || \
   find /usr/lib* -name "libcuda.so.1" 2>/dev/null | grep -q .; then
    echo "  ✅ NVIDIA driver (libcuda.so.1)"
else
    echo "  ❌ libcuda.so.1 — NVIDIA 驱动缺失（必需，无法安装）"
    OK=0
fi
# 注意：`set -e` + `pipefail` 下，探测类命令失败必须显式兜底（`|| true`），
# 否则"未检测到"会变成"安装中止"——例如全依赖版的目标系统本就没有 Qt，
# 或无 GPU/驱动不可访问时 nvidia-smi 返回非 0（实测 exit 9）。
QT_VER="$(pkg-config --modversion Qt6Quick 2>/dev/null || true)"
QT_MAJOR="${QT_VER%%.*}"
QT_MINOR="$(printf '%s' "$QT_VER" | cut -d. -f2)"
if [ -d "$SRC/lib/qt6" ]; then
    echo "  ✅ 捆绑 Qt 已随包（全依赖版）——无需系统 Qt"
elif [ -n "$QT_VER" ] && [ "${QT_MAJOR:-0}" -ge 6 ] && [ "${QT_MINOR:-0}" -ge 12 ]; then
    echo "  ✅ Qt $QT_VER (≥6.12)"
else
    echo "  ⚠ Qt 未检测到或 <6.12（需要 Qt ≥6.12；GUI 可能无法启动）"
    echo "     → 或源码构建自带 Qt 的包：./scripts/build_release.sh --variant=full（本项目不再分发预编译 full 包）"
fi
GPU_INFO="$(nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null | head -1 || true)"
# nvidia-smi 失败时把错误文本打到 stdout（非 stderr），只接受正常 CSV（含逗号）
case "$GPU_INFO" in
    *,*) ;;
    *) GPU_INFO="" ;;
esac
if [ -n "$GPU_INFO" ]; then
    CC="${GPU_INFO##*, }"
    if [ -n "$CC" ] && [ "${CC%%.*}" -ge 8 ] 2>/dev/null; then
        echo "  ✅ GPU: $GPU_INFO（Ampere+ → 插帧可用）"
    else
        echo "  ⚠ GPU: $GPU_INFO（Turing 及更早 → 插帧直通，VSR 不受影响）"
    fi
else
    echo "  ⚠ nvidia-smi 不可用（无 NVIDIA GPU？插帧/VSR 需要）"
fi
if [ "$OK" -eq 0 ]; then
    echo "必需依赖缺失，安装中止（详见上方 ❌）" >&2
    exit 1
fi

# ── 2. 复制自建部分 ──────────────────────────────────────────────────
echo "--- [2/6] install binaries + bundled libs ---"
mkdir -p "$BIN_DIR" "$LIB_DIR" "$FONT_DIR" "$BIN_DIR/translations"
cp "$GUI_BIN" "$BIN_DIR/vsr-player" && chmod +x "$BIN_DIR/vsr-player"
echo "  ✅ $BIN_DIR/vsr-player"
if [ -x "$CLI_BIN" ]; then
    cp "$CLI_BIN" "$BIN_DIR/mpv-vsr" && chmod +x "$BIN_DIR/mpv-vsr"
    echo "  ✅ $BIN_DIR/mpv-vsr"
fi
if [ -d "$LIB_SRC" ]; then
    cp -L "$LIB_SRC"/* "$LIB_DIR/"   # -L：跟随软链（VFX 的 ngx-vsr 软链等）
    echo "  ✅ 捆绑库（ffmpeg/CUDA/TRT/libmpv）→ $LIB_DIR/"
fi
# dev 模式：按 docs 把 wheel 的 nvidia_vfx.libs 放在 third_party/nvidia_vfx.libs，
# 安装时必须落到上面 RPATH 期望的位置——否则装出来的库加载失败 → VSR 静默直通。
if [ "$MODE" = dev ] && [ -d "$SRC/third_party/nvidia_vfx.libs" ]; then
    mkdir -p "$VFX_SIBLING"
    cp -L "$SRC"/third_party/nvidia_vfx.libs/*.so* "$VFX_SIBLING/" 2>/dev/null || true
    echo "  ✅ VFX cudart sibling（third_party/nvidia_vfx.libs）→ $VFX_SIBLING/"
fi
# Wayland 空闲抑制模块（可选，主程序运行时 dlopen；缺失时回退 D-Bus）。
# dev 树里在 build/src/client/；tarball 里已随 lib/vsr-player/ 一起拷过（LIB_SRC）。
IDLE_MOD="$SRC/build/src/client/libvsr-idle-wayland.so"
if [ -f "$IDLE_MOD" ]; then
    cp "$IDLE_MOD" "$LIB_DIR/"
    echo "  ✅ wayland idle-inhibit 模块 → $LIB_DIR/"
fi
# 全依赖版：捆绑的 Qt（库在 lib/，插件/QML 在 lib/qt6/）——镜像发行版布局，
# 使 Qt 插件自带的相对 RUNPATH（$ORIGIN/../../../）能解析到 <prefix>/lib。
QT_SRC="$SRC/lib/qt6"
if [ -d "$QT_SRC" ]; then
    PREFIX_DIR="$(dirname "$LIB_DIR")"          # ~/.local/lib
    mkdir -p "$PREFIX_DIR/qt6"
    cp -a "$QT_SRC/." "$PREFIX_DIR/qt6/"
    libs=0
    for so in "$SRC"/lib/libQt6*.so.*; do
        [ -e "$so" ] || continue
        cp -L "$so" "$PREFIX_DIR/" && libs=$((libs+1))
    done
    echo "  ✅ 捆绑 Qt（$libs 个库 + 插件/QML）→ $PREFIX_DIR/（无需系统 Qt）"
fi

# ── 3. engine + 字体 + 翻译 ─────────────────────────────────────────
echo "--- [3/6] engine + fonts + translations ---"
# 后端推断（**推断，不是元数据**）：没有 backend 标记文件，用已有信号判断——
# 捆绑了 libnvinfer.so.11 ⇒ 该包具备 RIFE/TensorRT 能力（rife 源码构建或 rife
# tarball），此时缺 engine 才是真问题；没有 TRT 库 ⇒ VFG-only 包（发行默认），
# 本来就不带 RIFE engine，不该告警。
# 判据必须看**本包**（$SRC/lib/vsr-player/libnvinfer* 或 $SRC/engines/）是否为 rife 后端，
# 不能看已安装目录：~/.local/lib/vsr-player/ 会累积上一版安装的残留（先装 rife 包、
# 再装 vfg 包时，旧 TRT 库还在，会把 VFG 包误判成"rife 包缺引擎"并误报告警）。
HAS_TRT=0
[ -f "$LIB_DIR/libnvinfer.so.11" ] && HAS_TRT=1
HAS_TRT_IN_PKG=0
[ -f "$SRC/lib/vsr-player/libnvinfer.so.11" ] && HAS_TRT_IN_PKG=1
if [ -f "$ENG_SRC/rife_full_fp16.engine" ]; then
    cp "$ENG_SRC/rife_full_fp16.engine" "$LIB_DIR/"
    echo "  ✅ RIFE engine（ampere+ 跨架构）→ $LIB_DIR/"
elif [ "$HAS_TRT_IN_PKG" -eq 1 ]; then
    echo "  ⚠ engine 缺失 — 插帧直通（build_release.sh 会构建）"
else
    echo "  ℹ VFG 后端：本包不含 TRT/engine，插帧走 VFX VideoFrameGeneration"
    [ "$HAS_TRT" -eq 1 ] && echo "     （注：$LIB_DIR/ 里有旧版 RIFE 的 TRT 库/引擎残留，不影响 VFG 使用）"
fi
FONT_SRC="$SRC/fonts/materialdesignicons-webfont.ttf"
[ -f "$FONT_SRC" ] || FONT_SRC="$SRC/third_party/material-icons/materialdesignicons-webfont.ttf"
if [ -f "$FONT_SRC" ]; then
    cp "$FONT_SRC" "$FONT_DIR/"
    echo "  ✅ 图标字体 → $FONT_DIR/"
fi
TR_SRC="$SRC/translations"
[ -d "$TR_SRC" ] || TR_SRC="$SRC/build/client-dist/src/client"
# dev 构建树：meson 的 lrelease custom_target 把 .qm 放在 build/src/client/
# （此前漏了这一档 → dev 安装沿用了上一版旧 .qm，新增 UI 文案静默回退英文）
[ -d "$TR_SRC" ] || TR_SRC="$SRC/build/src/client"
if ls "$TR_SRC"/*.qm >/dev/null 2>&1; then
    cp "$TR_SRC"/*.qm "$BIN_DIR/translations/"
    echo "  ✅ 翻译 → $BIN_DIR/translations/"
fi

# ── 4. 桌面集成（应用菜单/启动器入口 + 图标）─────────────────────────
# 入口内容 = data/vsr-player.desktop.in（单一事实源，含中英 Name/Comment），
# 这里只把 @EXEC@ 换成安装后的绝对路径（不依赖 PATH）。图标装进 hicolor
# （scalable SVG + 512 PNG，DE 自己挑）。缓存刷新是 best-effort：命令缺失
# （非 KDE / 无 gtk 工具）不算安装失败。
echo "--- [4/6] desktop integration ---"
DESKTOP_IN="$SRC/data/vsr-player.desktop.in"
APPS_DIR="$HOME/.local/share/applications"
ICON_BASE="$HOME/.local/share/icons/hicolor"
if [ -f "$DESKTOP_IN" ]; then
    mkdir -p "$APPS_DIR" "$ICON_BASE/scalable/apps" "$ICON_BASE/512x512/apps"
    sed "s|@EXEC@|$BIN_DIR/vsr-player|g" "$DESKTOP_IN" \
        > "$APPS_DIR/vsr-player.desktop"
    echo "  ✅ 应用入口 → $APPS_DIR/vsr-player.desktop"
    if [ -f "$SRC/data/vsr-player.svg" ]; then
        cp "$SRC/data/vsr-player.svg" "$ICON_BASE/scalable/apps/vsr-player.svg"
    fi
    if [ -f "$SRC/data/vsr-player.png" ]; then
        cp "$SRC/data/vsr-player.png" "$ICON_BASE/512x512/apps/vsr-player.png"
    fi
    echo "  ✅ 图标 → $ICON_BASE/{scalable,512x512}/apps/vsr-player.*"
    update-desktop-database "$APPS_DIR" 2>/dev/null || true
    gtk-update-icon-cache -q -t -f "$ICON_BASE" 2>/dev/null || true
    kbuildsycoca6 --noincremental 2>/dev/null \
        || kbuildsycoca5 --noincremental 2>/dev/null || true
else
    echo "  ⚠ 缺 $DESKTOP_IN（旧包不含 data/）→ 跳过桌面入口"
fi

# ── 5. VFX SDK（NVIDIA 官方 wheel，纯下载）──────────────────────────
# ⚠ 策略：**下载/提取失败一律报错退出（exit 1）**，不静默降级。
# 理由（2026-10-17 用户决定）：VFX 缺失时 VSR 会"静默直通"——脚本却 exit 0，
# 用户很容易以为装好了，超分其实没生效。要么装成，要么明确失败。
echo "--- [5/6] VFX SDK check ---"
vfx_fail() {   # VFX 不可用 → 明确报错并退出（手动方式一并给出）
    echo "  ❌ VFX SDK 不可用：$*" >&2
    echo "     手动方式：从 https://pypi.nvidia.com/nvidia-vfx/ 下载 manylinux x86_64 wheel，" >&2
    echo "     解压 nvvfx/libs/* → $LIB_DIR/、nvidia_vfx.libs/* → $VFX_SIBLING/，再重跑本脚本。" >&2
    echo "     （漏掉 nvidia_vfx.libs/ 会让 libVideoFXLocal.so 加载失败 → VSR 静默直通）" >&2
    exit 1
}
VFX_READY=0
if [ -f "$LIB_DIR/libNVCVImage.so" ]; then
    if ls "$VFX_SIBLING"/libcudart-*.so.* >/dev/null 2>&1; then
        VFX_READY=1
        echo "  ✅ VFX SDK 已就位（$LIB_DIR/ + $VFX_SIBLING/）"
    else
        # 老安装/手动放置：库在但 sibling 缺 → libVideoFXLocal.so 加载失败（VSR 静默直通）
        echo "  ⚠ VFX SDK 在 $LIB_DIR/ 但缺 $VFX_SIBLING/libcudart-*.so.*"
        echo "    （0.2.0.0 起的 wheel 把 cudart 放在该兄弟目录，VFX 库 RPATH 指向它）"
        echo "    → 重新下载 wheel 补齐（约 0.44 GB）"
    fi
fi
if [ "$VFX_READY" -eq 0 ]; then
    echo "  ⚠ VFX SDK 缺失 — 超分将直通（视频照常播放）"
    echo "    获取：NVIDIA 官方索引 nvidia-vfx wheel（内含 VFX 运行时，"
    echo "    许可：NVIDIA Software License Agreement 2025.05.05 ——"
    echo "    下载即与 NVIDIA 直接建立许可关系；本脚本仅代为下载文件，"
    echo "    不安装到 Python 环境，不向第三方分发）"
    if [ "$NO_VFX" -eq 1 ]; then
        echo "    （--no-vfx：跳过自动下载。手动方式见下方说明，下载后重跑本脚本即可）"
    elif command -v curl >/dev/null && command -v unzip >/dev/null; then
        echo "    正在从 NVIDIA 官方索引下载（约 0.6 GB，请耐心等待；Ctrl-C 可中断）..."
        TMPVFX="$(mktemp -d)"
        # wheel 来源：NVIDIA 官方索引 pypi.nvidia.com（PEP 503 简单索引，相对链接
        # 带 #sha256 片段）。⚠ pypi.org 上 nvidia-vfx 只有数 KB 的 sdist 占位、
        # 没有 manylinux wheel（2026-09 实测）——查 pypi.org 必然拿不到 URL。
        # 探测命令一律带 `|| true`：`set -e` + pipefail 下失败必须走到下面的提示
        # 分支；历史 bug 是静默中止，用户只看到"安装失败"，看不到手动方式。
        VFX_INDEX="https://pypi.nvidia.com/nvidia-vfx/"
        WHEEL_FILE="$(curl -fsSL "$VFX_INDEX" 2>/dev/null \
            | grep -oE 'href="[^"]*"' | sed 's/^href="//; s/"$//; s/#.*$//' \
            | grep -E 'manylinux.*x86_64\.whl$' | sort -V | tail -1 || true)"
        WHEEL_URL=""
        [ -n "$WHEEL_FILE" ] && WHEEL_URL="${VFX_INDEX}${WHEEL_FILE}"
        if [ -z "$WHEEL_URL" ]; then   # 兜底：上游若改回在 pypi.org 发布 wheel
            WHEEL_URL="$(curl -fsSL https://pypi.org/pypi/nvidia-vfx/json 2>/dev/null \
                | grep -oE '"url": *"[^"]*manylinux[^"]*\.whl"' | head -1 \
                | sed -E 's/"url": *"([^"]*)"/\1/' || true)"
        fi
        # wheel 文件名形如 nvidia_vfx-0.2.0.0-...-manylinux...whl，版本 = 第一段之后、
        # 第二个 "-" 之前；解析失败（pypi.org 兜底路径 WHEEL_FILE 为空/命名变化）静默留空
        VFX_WHEEL_VER=""
        case "$WHEEL_FILE" in
            nvidia_vfx-*)
                VFX_WHEEL_VER="${WHEEL_FILE#nvidia_vfx-}"
                VFX_WHEEL_VER="${VFX_WHEEL_VER%%-*}"
                ;;
        esac
        echo "    来源: ${WHEEL_URL:-（未找到可用 wheel）}${VFX_WHEEL_VER:+（VFX SDK $VFX_WHEEL_VER）}"
        if [ -n "$WHEEL_URL" ] && curl -fL "$WHEEL_URL" -o "$TMPVFX/nvidia_vfx.whl"; then
            unzip -o -q "$TMPVFX/nvidia_vfx.whl" "nvvfx/libs/*" -d "$TMPVFX"
            cp "$TMPVFX"/nvvfx/libs/*.so* "$LIB_DIR/" 2>/dev/null || true
            # libcudart-<hash>.so.* lives in nvidia_vfx.libs/, a sibling of nvvfx/ in the
            # wheel, not under nvvfx/libs/. The VFX libs carry RPATH
            # $ORIGIN/../../nvidia_vfx.libs, so from $LIB_DIR the only path that resolves is
            # <parent of LIB_DIR's parent>/nvidia_vfx.libs. Copying it next to the other VFX
            # libs does not work; without it libVideoFXLocal.so fails to load and VSR
            # silently passes through.
            # VFX_SIBLING 在脚本顶部（LIB_DIR 旁）定义——单一事实源
            # unzip exits 11 when a pattern matches nothing, and `set -e` would abort the whole
            # install on wheels that predate nvidia_vfx.libs/, so check the archive first.
            WHEEL_MEMBERS="$(unzip -Z1 "$TMPVFX/nvidia_vfx.whl" 2>/dev/null || true)"
            case "$WHEEL_MEMBERS" in
                *nvidia_vfx.libs/*)
                    unzip -o -q "$TMPVFX/nvidia_vfx.whl" "nvidia_vfx.libs/*" -d "$TMPVFX"
                    mkdir -p "$VFX_SIBLING"
                    cp "$TMPVFX"/nvidia_vfx.libs/*.so* "$VFX_SIBLING/" 2>/dev/null || true
                    ;;
                *nvvfx/libs/*) ;;   # 旧版 wheel（<0.2.0.0）：本来就没有 nvidia_vfx.libs/，跳过
                *) echo "  ⚠ wheel 目录清单读取异常，nvidia_vfx.libs 提取已跳过（若库不齐 VSR 将静默直通）" >&2 ;;
            esac
            # vsr_proc.c dlopen 无版本名（libnppc.so/libcudnn.so/...），wheel 只有
            # 带版本名（.so.12/.so.9/.so.1.8.2）——缺软链则 NEEDED 链断 → VFX 全灭
            # libnvidia-ngx-truehdr：VFX SDK 1.3（wheel 0.2.0.0）新增 TrueHDR effect，
            # wheel 只给带版本名（.so.1.2.3），播放器按无版本名 dlopen，约定同 ngx-vsr。
            # 旧 wheel 无此文件时下面的 [ -e "$s" ] 守卫直接跳过（无害）。
            for target in libnppc libnppial libnppicc libnppidei libnppig \
                          libnppif libnppim libnppist libnppitc libcudnn \
                          libnvidia-ngx-vsr libnvidia-ngx-truehdr; do
                src=""
                for s in "$LIB_DIR/${target}.so."*; do [ -e "$s" ] && src="$s" && break; done
                [ -n "$src" ] && ln -sf "$(basename "$src")" "$LIB_DIR/$target.so"
            done
            COUNT="$(ls "$LIB_DIR"/*.so 2>/dev/null | wc -l)"
            rm -rf "$TMPVFX"
            if [ "$COUNT" -ge 10 ]; then
                echo "  ✅ VFX SDK 下载并提取完成（$COUNT 个库）→ $LIB_DIR/"
            else
                vfx_fail "wheel 提取不完整（仅 $COUNT 个库）"
            fi
        else
            rm -rf "$TMPVFX"
            vfx_fail "下载失败（网络/代理）——wheel 未取到或下载被中断"
        fi
    else
        vfx_fail "缺少 curl 或 unzip，无法自动获取 wheel"
    fi
fi

# ── 5. 验证 + 摘要 ──────────────────────────────────────────────────
echo "--- [6/6] verify ---"
LDD_MISSING="$(ldd "$BIN_DIR/vsr-player" 2>/dev/null | grep "not found" | head -3 || true)"
if [ -n "$LDD_MISSING" ]; then
    echo "  ⚠ vsr-player 依赖缺失:"; echo "$LDD_MISSING"
fi
[ -f "$LIB_DIR/libnvinfer.so.11" ] && RIFE_TRT="✅" || RIFE_TRT="❌"
# 摘要里的 TRT 字段：无 TRT 库 = VFG-only 包（HAS_TRT 在上方 [3/6] 由同一信号推断），
# 显示后端名而不是看起来像故障的 "TRT:❌"。
if [ "$HAS_TRT" -eq 1 ]; then
    TRT_TAG="TRT:$RIFE_TRT"
else
    TRT_TAG="FRUC:VFG"
fi
{ [ -f "$LIB_DIR/libNVCVImage.so" ] && ls "$VFX_SIBLING"/libcudart-*.so.* >/dev/null 2>&1; } \
    && VFX_ST="✅" || VFX_ST="❌"
# VFX SDK 版本：离线从已解压目录推断（libnvVFXVideoFrameGeneration.so 是 1.3/0.2.0.0 新增），不联网
[ -f "$LIB_DIR/libnvVFXVideoFrameGeneration.so" ] && VFX_VER="1.3+ (0.2.0.0)" || VFX_VER="≤1.2"
echo ""
echo "=== Done ==="
echo "  GUI:  $BIN_DIR/vsr-player        [VFX:$VFX_ST $TRT_TAG]"
echo "  VFX:  $VFX_VER"
[ -x "$BIN_DIR/mpv-vsr" ] && echo "  CLI:  $BIN_DIR/mpv-vsr"
LIBS_DESC="捆绑依赖 + VFX"
# 描述按目录事实写：engine 文件在就打出来（与上面的后端标签解耦）。
if [ -f "$LIB_DIR/rife_full_fp16.engine" ]; then
    LIBS_DESC="捆绑依赖 + VFX + RIFE engine"
fi
echo "  库:   $LIB_DIR/（$LIBS_DESC）"
echo "  入口: 应用菜单/启动器 → VSR Player"
echo "运行: vsr-player <视频或目录>"
echo "（若 PATH 无 $BIN_DIR：export PATH=\"\$PATH:$BIN_DIR\"）"
