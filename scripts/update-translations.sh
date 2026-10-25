#!/bin/bash
# 更新 Qt 翻译（.ts）：一条命令跑完 lupdate + 完整性检查。
#
# ⚠ 为什么要有这个脚本：lupdate 只认**你显式给它的文件**。漏掉某个文件里的
#   `tr()/qsTr()/QT_TRANSLATE_NOOP()` → 对应条目变成 `type="vanished"` →
#   lrelease **不编进 .qm** → 运行时查不到 → 静默回退英文（2026-10-22 的
#   "UI 里插帧选项没做 i18n" 就是这么来的：`FrucBackend.h` 里的
#   `QT_TRANSLATE_NOOP("PlayerViewModel", …)` 没进 lupdate 输入）。
#
# 用法：
#   ./scripts/update-translations.sh          # 更新 .ts（之后手工补译文）
#   ./scripts/update-translations.sh --check  # 检查：源码里每个 UI 字面量都在 .ts 里，
#                                             # 且 zh_CN 没有 unfinished
#
# 补完译文后 `ninja -C build` 会经 lrelease 生成 <build>/src/client/*.qm。

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLIENT="$PROJECT_ROOT/src/client"
LUPDATE="${LUPDATE:-/usr/lib/qt6/bin/lupdate}"
LRCONVERT="${LCONVERT:-/usr/lib/qt6/bin/lconvert}"
TS_ZH="$CLIENT/translations/vsr-player_zh_CN.ts"
TS_EN="$CLIENT/translations/vsr-player_en.ts"

# 输入清单：QML（含独立 context 的 components）+ C++（含 QT_TRANSLATE_NOOP 的头）
# 新增带 UI 文案的文件时必须加到这里，否则条目会是 vanished。
INPUTS=(
    "$CLIENT/main.qml"
    "$CLIENT/StereoHotkeyHelp.qml"
    "$CLIENT"/ui/*.qml
    "$CLIENT"/ui/components/*.qml
    "$CLIENT/PlayerViewModel.cpp"
    "$CLIENT/FrucBackend.h"
)

# 源码里的 UI 字面量：tr("…") / qsTr("…") / QT_TRANSLATE_NOOP("ctx", "…")
# 这项检查针对的是"字面量压根没进 .ts"（漏文件 / tr(变量) 没配 NOOP 标记），
# 比 vanished 计数更准——lupdate -no-obsolete 会先把死条目清掉。
missing_literals() {
    local ts="$1"
    python3 - "$ts" "${INPUTS[@]}" <<'PYEOF'
import re, sys
ts, files = sys.argv[1], sys.argv[2:]
have = set(re.findall(r'<source>(.*?)</source>', open(ts, encoding='utf-8').read(), re.S))
pat = re.compile(r'(?<![A-Za-z0-9_])(?:qsTr|tr)\(\s*"((?:[^"\\]|\\.)*)"\s*\)|'
                 r'QT_TRANSLATE_NOOP\(\s*"[^"]*"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
missing, total = [], 0
for f in files:
    src = open(f, encoding='utf-8').read()
    for m in pat.finditer(src):
        lit = m.group(1) if m.group(1) is not None else m.group(2)
        if not lit:
            continue
        total += 1
        if lit not in have:
            missing.append(f"{f}: {lit!r}")
print(f"  源码字面量 {total} 个，缺条目 {len(missing)} 个")
for x in missing:
    print("    ✗", x)
sys.exit(1 if missing else 0)
PYEOF
}

if [[ "${1:-}" == "--check" ]]; then
    echo "翻译完整性检查："
    for ts in "$TS_ZH" "$TS_EN"; do
        printf "  %s: vanished=%s unfinished=%s\n" "$(basename "$ts")" \
            "$(grep -c 'type="vanished"' "$ts" || true)" \
            "$(grep -c 'type="unfinished"' "$ts" || true)"
    done
    if ! missing_literals "$TS_ZH"; then
        echo "✗ 有 UI 字面量没进 .ts：lrelease 不会编进 .qm，运行时会显示英文。" >&2
        exit 1
    fi
    # zh_CN 是唯一验收标准（en 允许 unfinished → 回退源串）
    if grep -q 'type="unfinished"' "$TS_ZH"; then
        echo "✗ zh_CN 存在 unfinished 条目（运行时会回退英文）" >&2
        exit 1
    fi
    echo "✓ zh_CN 完整"
    exit 0
fi

"$LUPDATE" -no-obsolete "${INPUTS[@]}" -ts "$TS_EN" "$TS_ZH"
echo
echo "更新完成，请补齐两份 .ts 的译文（zh_CN 必须 0 unfinished），然后："
echo "  ninja -C build && ./scripts/update-translations.sh --check"
