#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 双语 / 文档一致性扫描器（第11期·脚本化清扫）

用法（仓库根执行）：
    python3 docs/i18n_consistency_scan.py            # 人读报告（stdout）
    python3 docs/i18n_consistency_scan.py --json     # 机器可读
    python3 docs/i18n_consistency_scan.py --write    # 生成 docs/双语与文档一致性报告.md

扫描范围：src/shell/**  src/tui/**  src/alert/**  webui/**
分类：
  A 真·双语缺失   tr("S","S") 且 S 含 CJK           —— 英文界面会显示中文（必须修）
  B 语言中性      tr("S","S") 且 S 无 CJK           —— 标点/命令名/路径（正常，无需修）
  C 未包 tr 的中文 中文直接进 uart_puts/printf/fprintf(stdout)/ncplane_putstr
  D 未包 tr 的英文 ≥10 字符英文句子直接进同上
  E WebUI 可见中文（WebUI 无 i18n 机制——登记为"设计单语"，不强制改）
  F 命令面 ↔ 文档 不一致（dotted_cmd.c 命令表 vs docs/*.md 提及）

退出码：0 = 无 A 类；1 = 存在 A 类（可用于 CI 门禁）。
"""

import os
import re
import sys
import json
import glob
import collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCOPES = ["src/shell", "src/tui", "src/alert", "webui"]
REPORT_PATH = os.path.join(ROOT, "docs", "双语与文档一致性报告.md")

CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\u3000-\u303f\uff01-\uff60]")
TR = re.compile(r'tr\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')
SINK = re.compile(
    r"(uart_puts|puts|printf|fprintf\s*\(\s*std(?:out|err)|ncplane_putstr|ncplane_putstr_aligned)\s*\("
)
ASCII_SENT = re.compile(r"^[A-Za-z][A-Za-z0-9 ,.'!?:;()\-/%]{9,}$")
COMMENT = re.compile(r"^\s*(?:/\*|\*|//)")
# 明显不是用户文案的字符串（命令名/路径/格式串/键名）
NOISE = re.compile(r"^(?:/|--|\.\.|%[sd]|https?://|[a-z_]+\.[a-z_]+$|[A-Z_]+$)")
# 外部命令串（rm -rf '%s' / mkdir -p '%s' 等）——是 shell 参数而非用户文案，不算双语缺失
SHELL_CMD = re.compile(
    r"^(?:rm|mkdir|cp|mv|tar|chmod|chown|systemctl|python3?|sh|bash|dd|ln|kill|pkill|"
    r"curl|wget|sed|awk|grep|ls|cat|echo|mount|umount|sync|reboot|poweroff)\s"
)


def iter_files():
    for scope in SCOPES:
        base = os.path.join(ROOT, scope)
        for pat in ("**/*.c", "**/*.h", "**/*.html"):
            for p in glob.glob(os.path.join(base, pat), recursive=True):
                yield p


def rel(p):
    return os.path.relpath(p, ROOT)


def scan_text():
    findings = collections.defaultdict(list)
    for path in sorted(iter_files()):
        is_web = path.endswith(".html")
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue

        def lineno(pos):
            return text.count("\n", 0, pos) + 1

        # ---- A/B：tr(x, y)（支持跨行调用——整文件扫描，避免把多行 tr 误判为"未包 tr"）----
        for m in TR.finditer(text):
            en, zh = m.group(1), m.group(2)
            if en != zh:
                continue                          # 真双语
            item = (rel(path), lineno(m.start()), en)
            findings["A" if CJK.search(en) else "B"].append(item)

        # 用空格盖掉已识别的 tr(...) 片段（保留换行 → 行号不漂移）
        masked = TR.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)

        for i, line in enumerate(masked.splitlines(), 1):
            if COMMENT.match(line):
                continue
            if is_web:
                # ---- E：WebUI 可见中文（排除 HTML/JS 注释行）----
                raw = text.splitlines()[i - 1] if i - 1 < len(text.splitlines()) else ""
                if CJK.search(raw) and not raw.strip().startswith(("<!--", "//", "*")):
                    findings["E"].append((rel(path), i, raw.strip()[:110]))
                continue
            # ---- C/D：未包 tr 的字面量（只统计进入用户可见输出的）----
            if not SINK.search(line):
                continue
            for m in STR.finditer(line):
                s = m.group(1)
                if not s or NOISE.match(s) or SHELL_CMD.match(s):
                    continue
                if CJK.search(s):
                    findings["C"].append((rel(path), i, s[:110]))
                elif ASCII_SENT.match(s) and " " in s:
                    findings["D"].append((rel(path), i, s[:110]))
    return findings


def scan_commands():
    """F：dotted_cmd.c 命令表 ↔ docs/*.md 一致性"""
    table_path = os.path.join(ROOT, "src/shell/dotted_cmd.c")
    names, zh_labels = [], {}
    row = re.compile(r'\{\s*"([a-z0-9_.]+)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}')
    if os.path.exists(table_path):
        for line in open(table_path, encoding="utf-8", errors="replace"):
            m = row.search(line)
            if m:
                names.append(m.group(1))
                zh_labels[m.group(1)] = m.group(4)
    # 注意：排除本脚本生成的报告（否则它自身含全部命令名 → "docs 未提及"恒为 0，自我污染）
    docs_text = ""
    for md in sorted(glob.glob(os.path.join(ROOT, "docs", "*.md"))):
        if os.path.basename(md) == "双语与文档一致性报告.md":
            continue
        docs_text += open(md, encoding="utf-8", errors="replace").read() + "\n"
    # 反查：docs 里以 `a.b` 或 `a b`（点分/双词命令形态）出现、但命令表里没有的词
    doc_cmds = set(re.findall(r"`([a-z][a-z0-9_]*\.[a-z0-9_.]+)`", docs_text))
    doc_cmds |= set(re.findall(r"`([a-z][a-z0-9_]{1,14} [a-z0-9_ ]{2,24})`", docs_text))
    known_pairs = {n.replace(".", " ") for n in names}
    stale = sorted(c for c in doc_cmds if c not in names and c not in known_pairs)
    # ---- F2：文档「系统名 ↔ 命令前缀 ↔ 二进制」与代码/构建对齐 ----
    arch = ""
    arch_path = os.path.join(ROOT, "docs", "ARCHITECTURE_系统类目.md")
    if os.path.exists(arch_path):
        arch = open(arch_path, encoding="utf-8", errors="replace").read()
    doc_prefixes = set(re.findall(r"\b([a-z_]{2,12})\.\*", arch))
    code_prefixes = {n.split(".")[0] for n in names}
    mk = ""
    mk_path = os.path.join(ROOT, "Makefile")
    if os.path.exists(mk_path):
        mk = open(mk_path, encoding="utf-8", errors="replace").read()
    doc_bins = set(re.findall(r"`(lingos-[a-z0-9-]+)`", arch))
    bin_missing = sorted(b for b in doc_bins if b not in mk)

    return {
        "total": len(names),
        "names": names,
        "missing_in_docs": [n for n in names if n not in docs_text and n.split(".")[-1] not in docs_text],
        "doc_only_stale": stale,
        "sample_labels": {n: zh_labels[n] for n in names[:5]},
        "prefixes_doc_only": sorted(doc_prefixes - code_prefixes),
        "prefixes_code_only": sorted(code_prefixes - doc_prefixes),
        "doc_bins_missing_in_makefile": bin_missing,
    }


def main():
    args = set(sys.argv[1:])
    f = scan_text()
    cmd = scan_commands()
    order = ["A", "B", "C", "D", "E"]
    if "--json" in args:
        print(json.dumps({k: f.get(k, []) for k in order} | {"F": cmd},
                         ensure_ascii=False, indent=1))
    else:
        titles = {
            "A": "A 真·双语缺失（tr 同串且含中文——英文界面显示中文）",
            "B": "B 语言中性（tr 同串，无中文——正常）",
            "C": "C 未包 tr 的中文字面量",
            "D": "D 未包 tr 的英文句字面量",
            "E": "E WebUI 可见中文（无 i18n 机制——登记）",
        }
        for k in order:
            items = f.get(k, [])
            print(f"== {titles[k]}：{len(items)} 条 ==")
            if k in ("A", "C", "D"):
                for p, ln, s in items[:80]:
                    print(f"   {p}:{ln}  {s}")
                if len(items) > 80:
                    print(f"   …（其余 {len(items) - 80} 条见 --json）")
        print(f"== F 命令面 ↔ 文档：命令 {cmd['total']} 条，"
              f"docs 未提及 {len(cmd['missing_in_docs'])} 条，"
              f"docs 提到但代码无 {len(cmd.get('doc_only_stale', []))} 条 ==")
        for n in cmd["missing_in_docs"][:40]:
            print(f"   [docs 未提及] {n}")
        for n in cmd.get("doc_only_stale", [])[:40]:
            print(f"   [文档过时?] {n}")
        print(f"== F2 前缀：文档有但代码无 {cmd.get('prefixes_doc_only')} | "
              f"代码有但文档无 {cmd.get('prefixes_code_only')} ==")
        print(f"== F2 文档提到的二进制在 Makefile 中缺失：{cmd.get('doc_bins_missing_in_makefile')} ==")
    if "--write" in args:
        with open(REPORT_PATH, "w", encoding="utf-8") as fp:
            fp.write("# 双语与文档一致性报告（第11期·脚本化清扫）\n\n")
            fp.write("> 本文件由 `docs/i18n_consistency_scan.py --write` 生成，勿手改。\n\n")
            fp.write("## 摘要\n\n| 类别 | 数量 |\n|---|---|\n")
            for k in order:
                fp.write(f"| {k} | {len(f.get(k, []))} |\n")
            fp.write(f"| F 命令数 | {cmd['total']} |\n")
            fp.write(f"| F docs 未提及 | {len(cmd['missing_in_docs'])} |\n")
            fp.write(f"| F docs 提到但代码无（疑似过时文案） | {len(cmd.get('doc_only_stale', []))} |\n\n")
            for k in order:
                fp.write(f"## {k} 类（{len(f.get(k, []))}）\n\n")
                for p, ln, s in f.get(k, [])[:200]:
                    fp.write(f"- `{p}:{ln}` — {s}\n")
                fp.write("\n")
            fp.write("## F 命令面 ↔ 文档\n\n")
            fp.write("### F2 系统前缀 / 二进制 对齐\n\n")
            fp.write(f"- 文档有但命令表无的前缀：{cmd.get('prefixes_doc_only')}\n")
            fp.write(f"- 命令表有但文档无的前缀：{cmd.get('prefixes_code_only')}\n")
            fp.write(f"- 文档提到但 Makefile 无的二进制：{cmd.get('doc_bins_missing_in_makefile')}\n\n")
            for n in cmd["names"]:
                mark = "❌ 未提及" if n in cmd["missing_in_docs"] else "✅"
                fp.write(f"- {mark} `{n}`\n")
        print(f"[written] {rel(REPORT_PATH)}")
    return 1 if f.get("A") else 0


if __name__ == "__main__":
    sys.exit(main())
