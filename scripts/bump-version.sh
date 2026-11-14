#!/usr/bin/env bash
# 版本号唯一事实源：仓库 VERSION。每次推送至少 bump 一次 patch。
# 用法：scripts/bump-version.sh [patch|minor|major] (默认 patch)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KIND="${1:-patch}"
[ -f "$ROOT/VERSION" ] || { echo 0.13.0 > "$ROOT/VERSION"; }
IFS=. read -r MAJ MIN PAT < "$ROOT/VERSION"
case "$KIND" in
  major) MAJ=$((MAJ+1)); MIN=0; PAT=0 ;;
  minor) MIN=$((MIN+1)); PAT=0 ;;
  patch) PAT=$((PAT+1)) ;;
  *) echo "用法：$0 [patch|minor|major]"; exit 2 ;;
esac
NEW="$MAJ.$MIN.$PAT"
printf '%s\n' "$NEW" > "$ROOT/VERSION"
echo "VERSION -> $NEW"
echo "提示：CI 打包会自动追加 +<short-sha>，保证每次推送的产物文件名不同。"
