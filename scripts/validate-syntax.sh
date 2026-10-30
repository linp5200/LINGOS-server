#!/bin/bash
# LING OS 语法/静态校验（0.10）
set -u
cd "$(dirname "$0")/.." || exit 1
fail=0
note() { printf '%s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; fail=1; }

note "== shell 语法 =="
while IFS= read -r f; do
    if head -1 "$f" 2>/dev/null | grep -qE '^#!.*(bash|sh)'; then
        bash -n "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }
    fi
done < <(find distro scripts -type f -not -path '*/.git/*' 2>/dev/null)
for f in distro/desktop/skel/.bash_profile; do
    [ -f "$f" ] && { bash -n "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }; }
done

note "== Python 语法 =="
while IFS= read -r f; do
    python3 -m py_compile "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }
done < <(find . -type f -name '*.py' -not -path './.git/*')

note "== JSON =="
while IFS= read -r f; do
    python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }
done < <(find . -type f -name '*.json' -not -path './.git/*')

note "== YAML =="
while IFS= read -r f; do
    python3 -c 'import yaml,sys; yaml.safe_load(open(sys.argv[1]))' "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }
done < <(find .github -type f \( -name '*.yml' -o -name '*.yaml' \) 2>/dev/null)

note "== XML =="
if command -v xmllint >/dev/null 2>&1; then
    while IFS= read -r f; do
        xmllint --noout "$f" 2>/tmp/syntax.err || { bad "$f"; cat /tmp/syntax.err; }
    done < <(find distro -type f -name '*.xml' 2>/dev/null)
else
    note "xmllint 不存在，跳过 XML"
fi

note "== Desktop Entry =="
while IFS= read -r f; do
    head -1 "$f" | grep -q '^\[Desktop Entry\]' || bad "$f: 缺少 [Desktop Entry]"
done < <(find distro/desktop/skel/Desktop -type f -name '*.desktop' 2>/dev/null)

if [ "$fail" = "1" ]; then
    note "== 校验失败 =="
    exit 1
fi
note "== 全部通过 =="
