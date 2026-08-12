#!/usr/bin/env bash
# install.sh — 用户端安装 vsr-player（GUI + CLI + 捆绑依赖）。
#
# 环境纯净原则：
#   - 只复制文件到应用自属目录（~/.local/bin + ~/.local/lib/vsr-player/）
#   - 系统依赖（Qt/driver）只检测提示，绝不自动安装
#   - VFX 获取：curl 从 PyPI 官方 wheel 纯下载 + 解压（不调用 pip 安装器，
#     不碰 Python 环境）；失败则保留提示，可手动下载后重跑
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
#   ~/.local/bin/translations/              # 翻译（main.cpp fallback）
set -euo pipefail

NO_VFX=0
[ "${1:-}" = "--no-vfx" ] && NO_VFX=1

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$HOME/.local/bin"
LIB_DIR="$HOME/.local/lib/vsr-player"
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
echo "--- [1/5] system check ---"
OK=1
if ldconfig -p 2>/dev/null | grep -q "libcuda\.so\.1" || \
   find /usr/lib* -name "libcuda.so.1" 2>/dev/null | grep -q .; then
    echo "  ✅ NVIDIA driver (libcuda.so.1)"
else
    echo "  ❌ libcuda.so.1 — NVIDIA 驱动缺失（必需，无法安装）"
    OK=0
fi
QT_MAJOR="$(pkg-config --modversion Qt6Quick 2>/dev/null | cut -d. -f1)"
QT_MINOR="$(pkg-config --modversion Qt6Quick 2>/dev/null | cut -d. -f2)"
if [ "${QT_MAJOR:-0}" -ge 6 ] && [ "${QT_MINOR:-0}" -ge 11 ]; then
    echo "  ✅ Qt $(pkg-config --modversion Qt6Quick) (≥6.11)"
else
    echo "  ⚠ Qt 未检测到或 <6.11（需要 Qt ≥6.11；GUI 可能无法启动）"
fi
GPU_INFO="$(nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null | head -1)"
if [ -n "$GPU_INFO" ]; then
    CC="${GPU_INFO##*, }"
    if [ "$(echo "$CC" | cut -d. -f1)" -ge 8 ]; then
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
echo "--- [2/5] install binaries + bundled libs ---"
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

# ── 3. engine + 字体 + 翻译 ─────────────────────────────────────────
echo "--- [3/5] engine + fonts + translations ---"
if [ -f "$ENG_SRC/rife_full_fp16.engine" ]; then
    cp "$ENG_SRC/rife_full_fp16.engine" "$LIB_DIR/"
    echo "  ✅ RIFE engine（ampere+ 跨架构）→ $LIB_DIR/"
else
    echo "  ⚠ engine 缺失 — 插帧直通（build_release.sh 会构建）"
fi
FONT_SRC="$SRC/fonts/materialdesignicons-webfont.ttf"
[ -f "$FONT_SRC" ] || FONT_SRC="$SRC/third_party/material-icons/materialdesignicons-webfont.ttf"
if [ -f "$FONT_SRC" ]; then
    cp "$FONT_SRC" "$FONT_DIR/"
    echo "  ✅ 图标字体 → $FONT_DIR/"
fi
TR_SRC="$SRC/translations"
[ -d "$TR_SRC" ] || TR_SRC="$SRC/build/client-dist/src/client"
if ls "$TR_SRC"/*.qm >/dev/null 2>&1; then
    cp "$TR_SRC"/*.qm "$BIN_DIR/translations/"
    echo "  ✅ 翻译 → $BIN_DIR/translations/"
fi

# ── 4. VFX SDK（NVIDIA 官方 wheel，纯下载）──────────────────────────
echo "--- [4/5] VFX SDK check ---"
if [ -f "$LIB_DIR/libNVCVImage.so" ]; then
    echo "  ✅ VFX SDK 已就位（$LIB_DIR/）"
else
    echo "  ⚠ VFX SDK 缺失 — 超分将直通（视频照常播放）"
    echo "    获取：NVIDIA 官方 PyPI 包 nvidia-vfx（wheel 内含 VFX 运行时，"
    echo "    许可：NVIDIA Software License Agreement 2025.05.05 ——"
    echo "    下载即与 NVIDIA 直接建立许可关系；本脚本仅代为下载文件，"
    echo "    不安装到 Python 环境，不向第三方分发）"
    if [ "$NO_VFX" -eq 1 ]; then
        echo "    （--no-vfx：跳过自动下载。手动方式见下方说明，下载后重跑本脚本即可）"
    elif command -v curl >/dev/null && command -v unzip >/dev/null; then
        echo "    正在从 PyPI 下载（~1.1GB，请耐心等待；Ctrl-C 可中断）..."
        TMPVFX="$(mktemp -d)"
        WHEEL_URL="$(curl -fsSL https://pypi.org/pypi/nvidia-vfx/json 2>/dev/null \
            | grep -oE '"url": *"[^"]*manylinux[^"]*\.whl"' | head -1 \
            | sed -E 's/"url": *"([^"]*)"/\1/')"
        if [ -n "$WHEEL_URL" ] && curl -fL "$WHEEL_URL" -o "$TMPVFX/nvidia_vfx.whl"; then
            unzip -o -q "$TMPVFX/nvidia_vfx.whl" "nvvfx/libs/*" -d "$TMPVFX"
            cp "$TMPVFX"/nvvfx/libs/*.so* "$LIB_DIR/" 2>/dev/null || true
            # vsr_proc.c dlopen 无版本名（libnppc.so/libcudnn.so/...），wheel 只有
            # 带版本名（.so.12/.so.9/.so.1.8.2）——缺软链则 NEEDED 链断 → VFX 全灭
            for target in libnppc libnppial libnppicc libnppidei libnppig \
                          libnppif libnppim libnppist libnppitc libcudnn \
                          libnvidia-ngx-vsr; do
                src=""
                for s in "$LIB_DIR/${target}.so."*; do [ -e "$s" ] && src="$s" && break; done
                [ -n "$src" ] && ln -sf "$(basename "$src")" "$LIB_DIR/$target.so"
            done
            COUNT="$(ls "$LIB_DIR"/*.so 2>/dev/null | wc -l)"
            rm -rf "$TMPVFX"
            if [ "$COUNT" -ge 10 ]; then
                echo "  ✅ VFX SDK 下载并提取完成（$COUNT 个库）→ $LIB_DIR/"
            else
                echo "  ⚠ VFX 提取不完整（$COUNT 库）——请手动下载后重跑脚本"
            fi
        else
            rm -rf "$TMPVFX"
            echo "  ❌ PyPI 下载失败（网络/代理？）。手动方式："
            echo "     curl -LO https://pypi.org/project/nvidia-vfx/ 或浏览器下载 wheel，"
            echo "     解压 nvvfx/libs/* 到 $LIB_DIR/ 后重跑本脚本"
        fi
    else
        echo "  （需要 curl + unzip 才能自动下载；手动下载 wheel 解压到 $LIB_DIR/ 亦可）"
    fi
fi

# ── 5. 验证 + 摘要 ──────────────────────────────────────────────────
echo "--- [5/5] verify ---"
LDD_MISSING="$(ldd "$BIN_DIR/vsr-player" 2>/dev/null | grep "not found" | head -3 || true)"
if [ -n "$LDD_MISSING" ]; then
    echo "  ⚠ vsr-player 依赖缺失:"; echo "$LDD_MISSING"
fi
[ -f "$LIB_DIR/libnvinfer.so.11" ] && RIFE_TRT="✅" || RIFE_TRT="❌"
[ -f "$LIB_DIR/libNVCVImage.so" ] && VFX_ST="✅" || VFX_ST="❌"
echo ""
echo "=== Done ==="
echo "  GUI:  $BIN_DIR/vsr-player        [VFX:$VFX_ST TRT:$RIFE_TRT]"
[ -x "$BIN_DIR/mpv-vsr" ] && echo "  CLI:  $BIN_DIR/mpv-vsr"
echo "  库:   $LIB_DIR/（捆绑依赖 + engine + VFX）"
echo "运行: vsr-player <视频或目录>"
echo "（若 PATH 无 $BIN_DIR：export PATH=\"\$PATH:$BIN_DIR\"）"
