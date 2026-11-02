#!/bin/bash
# precommit.sh — 提交前检查（i18n 为主，一条命令跑完）。
#
# 为什么需要它：UI 文案漏进 .ts 是**静默故障**——lupdate 没吃到某个文件 →
# 条目不存在 / 类型是 vanished → lrelease 不编进 .qm → 运行时回退英文，
# 编译、运行、截图都看不出来（2026-10-22「插帧选项没做 i18n」、2026-11-02
# 「Subtitle tracks 漏译」都是这一类）。
#
# 用法：
#   ./scripts/precommit.sh                # 键位表 → 更新 .ts → lrelease → 完整性检查
#   ./scripts/precommit.sh --no-build     # 跳过 ninja（只查键位表与 .ts，快）
#   ./scripts/precommit.sh --install-hook # 装成 .git/hooks/pre-commit（软链）
#
# 退出码：0 = 可以提交；1 = 需要处理（输出里会写清楚改什么）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DO_BUILD=1
INSTALL_HOOK=0
for arg in "$@"; do
    case "$arg" in
        --no-build)     DO_BUILD=0 ;;
        --install-hook) INSTALL_HOOK=1 ;;
        -h|--help)      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

if [ "$INSTALL_HOOK" = 1 ]; then
    HOOK="$ROOT/.git/hooks/pre-commit"
    if [ ! -d "$ROOT/.git/hooks" ]; then
        echo "✗ 没有 .git/hooks（不是 git 仓库？）" >&2
        exit 1
    fi
    if [ -e "$HOOK" ] && [ ! -L "$HOOK" ]; then
        echo "✗ $HOOK 已存在且不是软链，拒绝覆盖（先自己备份/删掉）" >&2
        exit 1
    fi
    ln -sfn "$(realpath --relative-to="$ROOT/.git/hooks" "$ROOT/scripts/precommit.sh")" "$HOOK"
    echo "✓ 已安装 pre-commit 钩子 → $HOOK"
    exit 0
fi

FAIL=0
say()  { printf '%s\n' "$*"; }
fail() { printf '  ✗ %s\n' "$*"; FAIL=1; }

say "── 1/5 键位表静态检查（重复按键 / 未实现动作 / 键位回流 QML）──────"
# 快捷键的 bug 几乎都是"同一件事写了多份、漏了一处"：QML 里多一条 Shortcut、
# 菜单里另抄一份偏移键映射、某个 id 没人实现。编译/启动/截图都看不出来，只有
# 用户按到那个组合才发现——所以放在最前面，改键位时第一时间拦住。
if ./tests/hotkeys/check_keys.py; then
    say "  ✓ 键位表一致"
else
    fail "键位表检查未通过（见上方输出）"
fi

say "── 2/5 更新 .ts（lupdate）────────────────────────────────────"
if ! ./scripts/update-translations.sh; then
    fail "update-translations.sh 执行失败"
fi

# 只看"非 location"的 diff：lupdate 每次都会刷新所有 <location line=…>
#（改了任何 QML 行号都会变），那不算文案变化；否则每次改注释都会误报。
diff_all="$(git diff -- src/client/translations | grep -E '^[+-]' | grep -vE '^(\+\+\+|---)' || true)"
diff_real="$(printf '%s\n' "$diff_all" | grep -vE '<location ' || true)"
if [ -n "$diff_all" ] && [ -z "$diff_real" ]; then
    say "  ✓ 只有 <location> 行号刷新（无文案变化）"
fi

if [ -n "$diff_real" ]; then
    fail "lupdate 改动了 .ts 的**文案条目** —— 有新增/变更的 UI 文案，必须补译文"
    say "    受影响文件："
    git status --porcelain -- src/client/translations | sed 's/^/      /'
    say "    未完成条目："
    python3 - <<'PY'
import pathlib, re
for f in ("src/client/translations/vsr-player_zh_CN.ts",
          "src/client/translations/vsr-player_en.ts"):
    p = pathlib.Path(f)
    if not p.exists():
        continue
    s = p.read_text()
    ctx = None
    for m in re.finditer(r'<name>(.*?)</name>|<source>(.*?)</source>\s*<translation([^>]*)>(.*?)</translation>', s, re.S):
        if m.group(1) is not None:
            ctx = m.group(1)
        elif 'type="unfinished"' in m.group(3):
            print("      [%s] %s" % (ctx, m.group(2)))
PY
fi

if [ "$DO_BUILD" = 1 ]; then
    say "── 3/5 lrelease（ninja -C build）─────────────────────────────"
    if ! ninja -C build >/tmp/precommit-ninja.log 2>&1; then
        fail "构建失败，日志尾巴："
        tail -n 15 /tmp/precommit-ninja.log | sed 's/^/      /'
    else
        say "  ✓ 构建通过（.qm 已刷新）"
    fi
else
    say "── 3/5 跳过构建（--no-build）─────────────────────────────────"
fi

say "── 4/5 完整性检查（--check）───────────────────────────────────"
if ./scripts/update-translations.sh --check; then
    say "  ✓ 字面量/译文完整"
else
    fail "完整性检查未通过（见上方输出）"
fi

say "── 5/5 运行期抽查（.qm 里反查新文案）──────────────────────────"
QM="build/src/client/vsr-player_zh_CN.qm"
if [ ! -f "$QM" ]; then
    fail "$QM 不存在（构建产物缺失）"
else
    LCONVERT="${LCONVERT:-/usr/lib/qt6/bin/lconvert}"
    missing=0
    for s in "轨道" "网络流" "字幕轨道" "附近字幕"; do
        if ! "$LCONVERT" -i "$QM" -o - 2>/dev/null | grep -q "$s"; then
            fail ".qm 里查不到「$s」——译文没编进 lrelease 产物"
            missing=1
        fi
    done
    [ "$missing" = 0 ] && say "  ✓ 菜单相关译文在 .qm 里可查到"
fi

if [ "$FAIL" = 0 ]; then
    say ""
    say "✓ 可以提交"
    exit 0
fi
say ""
say "✗ 有检查未通过 —— 处理完再提交（补译文：src/client/translations/*.ts）"
exit 1
