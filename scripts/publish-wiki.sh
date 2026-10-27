#!/usr/bin/env bash
# publish-wiki.sh — 把仓库里的 wiki/ 同步到 GitHub wiki（*.wiki.git）
#
# 背景：GitHub 的 wiki 仓库是**惰性创建**的——在仓库设置里勾上 "Wikis" 只会多出一个
# 空的 Wiki 标签页，`<repo>.wiki.git` 要等**第一次在网页上保存 wiki 页面**之后才存在。
# 本脚本先探测，探测不到就打印出该做的那两步，而不是抛一个 git 报错。
#
# 用法: ./scripts/publish-wiki.sh [--dry-run]
set -euo pipefail

REPO_SLUG="${VSR_WIKI_SLUG:-zhangmq/vsr-player}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/wiki"
URL="git@github.com:${REPO_SLUG}.wiki.git"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

[ -d "$SRC" ] || { echo "❌ 找不到 $SRC（wiki 源目录）" >&2; exit 1; }

if ! git ls-remote "$URL" >/dev/null 2>&1; then
    cat <<EOF
❌ wiki 仓库还不存在：$URL

   GitHub 只在**第一次保存 wiki 页面之后**才创建 \`*.wiki.git\`。
   启用 wiki（Settings → Features → Wikis）只是让 Wiki 标签页出现，仓库仍是空的。

   请先做一次：
     1) 打开 https://github.com/${REPO_SLUG}/wiki
     2) 点 “Create the first page”，标题保持 Home，随便写一句，点 Save
     3) 重新运行本脚本（首次发布会把那个占位页覆盖成真正的主页）
EOF
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
git clone --depth 1 --quiet "$URL" "$TMP"

# wiki/ 里的 README.md 是"怎么发布"的说明，不属于 wiki 页面
for f in "$SRC"/*.md; do
    case "$(basename "$f")" in
        README.md) continue ;;
    esac
    cp "$f" "$TMP/"
done

cd "$TMP"
git add -A
if git diff --cached --quiet; then
    echo "✅ wiki 已是最新，无需推送"
    exit 0
fi
git -c user.name="${GIT_AUTHOR_NAME:-$(git -C "$ROOT" config user.name || echo vsr-player)}" \
    -c user.email="${GIT_AUTHOR_EMAIL:-$(git -C "$ROOT" config user.email || echo vsr-player@localhost)}" \
    commit --quiet -m "docs: sync wiki from repository ($(date +%F))"

if [ "$DRY" -eq 1 ]; then
    echo "（--dry-run）已生成提交，未推送。页面："
    git show --stat --oneline HEAD | tail -n +2
    exit 0
fi

git push --quiet origin HEAD
echo "✅ 已发布：https://github.com/${REPO_SLUG}/wiki"
