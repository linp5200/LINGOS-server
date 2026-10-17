# -*- coding: utf-8 -*-
"""示例 ingest 插件：本地 Markdown 文档解析

插件约定：
    manifest.json —— 元数据（id/type/name/version/source_types/entry/sandbox/...）
    main.py       —— 必须提供 parse(source: dict) -> dict
                     source: {"source": <路径或 URL>, "title": "", "text": "", meta: {}}
                     返回:   {"title": str, "text": str, "meta": dict}

安装：把整个目录拷到 /LINGOS/plugins/ingest/ 下，系统自动发现（无需改代码）。
"""
import os
import re


def parse(source):
    path = str(source.get("source") or "")
    if not os.path.isfile(path):
        raise RuntimeError("文件不存在: %s" % path)
    with open(path, encoding="utf-8", errors="ignore") as f:
        raw = f.read(2 * 1024 * 1024)
    text = re.sub(r"^\s{0,3}#{1,6}\s*", "", raw, flags=re.M)
    text = re.sub(r"\*\*(.+?)\*\*", r"\1", text)
    text = re.sub(r"\x60{1,3}([^\x60]*)\x60{1,3}", r"\1", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    title = os.path.basename(path)
    for ln in raw.splitlines():
        if ln.strip().startswith("#"):
            title = ln.strip().lstrip("#").strip() or title
            break
    return {"title": title, "text": text, "meta": {"path": path}}
