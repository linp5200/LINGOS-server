#!/usr/bin/env python3
"""从 assets/tokens.json（DTCG）生成 tokens.css；单一事实源，禁止手改产物。"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "webdesk" / "assets" / "tokens.json"
OUT = ROOT / "webdesk" / "assets" / "tokens.css"


def walk(node, prefix=()):
    if isinstance(node, dict) and "$type" in node and "$value" in node:
        yield prefix, node["$type"], node["$value"]
        return
    if isinstance(node, dict):
        for k, v in node.items():
            if k.startswith("$"):
                continue
            yield from walk(v, prefix + (k,))


def main():
    data = json.loads(SRC.read_text(encoding="utf-8"))
    lines = ["/* 由 scripts/build-tokens.py 从 tokens.json 生成，请勿手改。 */", ":root{"]
    for path, typ, val in walk(data):
        name = "--" + "-".join(path)
        if typ == "color":
            lines.append(f"  {name}: {val};")
            if len(path) == 2 and path[0] == "color":
                lines.append(f"  --{path[1]}: {val};")
        elif typ in ("dimension", "duration"):
            lines.append(f"  {name}: {val};")
            if len(path) == 2 and path[0] == "radius":
                lines.append(f"  --r-{path[1]}: {val};")
            if len(path) == 2 and path[0] == "duration":
                lines.append(f"  --dur-{path[1]}: {val};")
        elif typ == "typography" and isinstance(val, dict):
            lines.append(f"  {name}-size: {val.get('fontSize', '13px')};")
            lines.append(f"  {name}-weight: {val.get('fontWeight', 400)};")
            lines.append(f"  {name}-line: {val.get('lineHeight', 1.5)};")
        else:
            sys.exit(f"未知 token 类型: {path} {typ}")
    lines.append("}")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({len(lines)-2} tokens)")


if __name__ == "__main__":
    main()
