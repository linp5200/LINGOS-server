#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 外部内容接入/解析（S16 Ingest Pipeline）

一个插件 = 一类内容源。系统统一负责：来源 -> 队列 -> 插件解析 -> 规范化 -> 切块
-> 入库 -> 检索/记忆。

命令面（EXT_MAP 自动接入 /api/cmd 与 App/WebUI）：
    ingest_add      添加内容（URL/文件路径/文本）
    ingest_status   任务状态
    ingest_list     已入库文档
    ingest_search   关键词检索
    ingest_plugins  可用插件清单
    ingest_delete   删除文档

用户插件目录：/LINGOS/plugins/ingest/<id>/{manifest.json, main.py}
内置插件：webpage / text / pdf / rss / audio / ocr（缺依赖时诚实报错，不伪造）

注意（预留）：插件沙箱/网络白名单/超时目前为进程内 try+timeout 的过渡实现，
后续接 app_sandbox（seccomp/namespace）。TODO(seam:S16)
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
from html.parser import HTMLParser
from typing import Any, Dict, List, Optional

try:
    import urllib.request
    import urllib.error
except Exception:  # pragma: no cover
    urllib = None

ROOT = os.environ.get("LINGOS_INGEST_ROOT", "/LINGOS/data/ingest")
DOCS_DIR = os.path.join(ROOT, "docs")
JOBS_DIR = os.path.join(ROOT, "jobs")
INDEX_PATH = os.path.join(ROOT, "index.json")
PLUGIN_DIR = os.environ.get("LINGOS_INGEST_PLUGINS", "/LINGOS/plugins/ingest")

_lock = threading.RLock()
_jobs: Dict[str, Dict[str, Any]] = {}
_plugins: Dict[str, Dict[str, Any]] = {}

MAX_TEXT = 400000          # 单文档文本上限（字符）
CHUNK_CHARS = 1200         # 每块字符数（约 800 token）
CHUNK_OVERLAP = 120


def _ensure_dirs() -> None:
    for d in (ROOT, DOCS_DIR, JOBS_DIR):
        try:
            os.makedirs(d, exist_ok=True)
        except Exception:
            pass


def _now() -> int:
    return int(time.time())


def _safe_id(prefix: str) -> str:
    return "%s_%s_%s" % (prefix, time.strftime("%Y%m%d_%H%M%S"), uuid.uuid4().hex[:8])


# ============================================================
# 内置解析器（插件等价物）
# ============================================================

class _TextExtractor(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts: List[str] = []
        self._skip = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style", "noscript", "svg"):
            self._skip += 1
        if tag in ("p", "br", "div", "li", "h1", "h2", "h3", "h4", "tr"):
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag in ("script", "style", "noscript", "svg") and self._skip > 0:
            self._skip -= 1
        if tag in ("p", "div", "li", "h1", "h2", "h3", "h4", "tr"):
            self.parts.append("\n")

    def handle_data(self, data):
        if self._skip == 0 and data.strip():
            self.parts.append(data.strip())

    def text(self) -> str:
        raw = " ".join(self.parts)
        raw = re.sub(r"[ \t\r\f\v]+", " ", raw)
        raw = re.sub(r"\n\s*\n+", "\n\n", raw)
        return raw.strip()


def _fetch_url(url: str, timeout: int = 15) -> Optional[bytes]:
    if urllib is None:
        return None
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "LINGOS-Ingest/1.0 (+local)",
            "Accept-Language": "zh-CN,zh,en",
        })
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read(3 * 1024 * 1024)
    except Exception:
        return None


def parse_webpage(source: Dict[str, Any]) -> Dict[str, Any]:
    url = str(source.get("source") or "")
    raw = _fetch_url(url)
    if not raw:
        raise RuntimeError("网页抓取失败（网络不可达或 URL 非法）")
    html = raw.decode("utf-8", errors="ignore")
    m = re.search(r"<title[^>]*>(.*?)</title>", html, re.S | re.I)
    title = (m.group(1).strip() if m else url)[:200]
    p = _TextExtractor()
    try:
        p.feed(html)
    except Exception:
        pass
    text = p.text()
    if len(text) < 30:
        raise RuntimeError("未能从页面提取到正文（可能是 JS 渲染页）")
    return {"title": title, "text": text, "meta": {"url": url, "bytes": len(raw)}}


def parse_text(source: Dict[str, Any]) -> Dict[str, Any]:
    text = str(source.get("text") or source.get("source") or "")
    if not text.strip():
        raise RuntimeError("文本为空")
    title = str(source.get("title") or text.strip().splitlines()[0][:80])
    return {"title": title, "text": text, "meta": {}}


def _which(cmd: str) -> Optional[str]:
    return shutil.which(cmd)


def parse_pdf(source: Dict[str, Any]) -> Dict[str, Any]:
    path = str(source.get("source") or "")
    if not os.path.isfile(path):
        raise RuntimeError("PDF 文件不存在: %s" % path)
    tool = _which("pdftotext")
    if not tool:
        raise RuntimeError("缺少依赖 pdftotext（poppler-utils）；请先安装后重试")
    try:
        out = subprocess.run([tool, "-layout", path, "-"], capture_output=True, timeout=60)
    except Exception as e:
        raise RuntimeError("pdftotext 执行失败: %s" % e)
    text = (out.stdout or b"").decode("utf-8", errors="ignore").strip()
    if not text:
        raise RuntimeError("PDF 无可提取文本（可能是扫描件，请用 OCR 插件）")
    title = os.path.basename(path)
    return {"title": title, "text": text, "meta": {"path": path}}


def parse_rss(source: Dict[str, Any]) -> Dict[str, Any]:
    url = str(source.get("source") or "")
    raw = _fetch_url(url)
    if not raw:
        raise RuntimeError("订阅源抓取失败")
    import xml.etree.ElementTree as ET
    try:
        root = ET.fromstring(raw)
    except Exception as e:
        raise RuntimeError("RSS/Atom 解析失败: %s" % e)
    items = []
    for it in root.iter():
        tag = it.tag.split("}")[-1].lower()
        if tag in ("item", "entry"):
            title = ""
            link = ""
            desc = ""
            for ch in it:
                t = ch.tag.split("}")[-1].lower()
                if t == "title" and ch.text:
                    title = ch.text.strip()
                elif t == "link" and (ch.text or ch.get("href")):
                    link = (ch.text or ch.get("href") or "").strip()
                elif t in ("description", "summary", "content") and ch.text:
                    desc = ch.text.strip()
            if title or desc:
                items.append({"title": title, "link": link, "desc": desc[:500]})
    if not items:
        raise RuntimeError("订阅源中没有条目")
    text = "\n\n".join("%d. %s\n%s" % (i + 1, x["title"], x["desc"]) for i, x in enumerate(items[:50]))
    return {"title": "RSS: " + url[:80], "text": text, "meta": {"url": url, "items": len(items)}}


def parse_audio(source: Dict[str, Any]) -> Dict[str, Any]:
    path = str(source.get("source") or "")
    if not os.path.isfile(path):
        raise RuntimeError("音频文件不存在: %s" % path)
    whisper = _which("whisper-cli") or _which("main")
    model = os.environ.get("LINGOS_WHISPER_MODEL", "")
    if not whisper or not model:
        raise RuntimeError("语音转写未就绪：需要 whisper.cpp（whisper-cli/main）+ LINGOS_WHISPER_MODEL 模型路径")
    try:
        out = subprocess.run([whisper, "-m", model, "-f", path, "-nt"], capture_output=True, timeout=600)
    except Exception as e:
        raise RuntimeError("转写执行失败: %s" % e)
    text = (out.stdout or b"").decode("utf-8", errors="ignore")
    lines = [re.sub(r"^\[[^\]]*\]\s*", "", ln).strip() for ln in text.splitlines()]
    text = "\n".join(x for x in lines if x)
    if not text:
        raise RuntimeError("转写结果为空")
    return {"title": "音频转写: " + os.path.basename(path), "text": text, "meta": {"path": path}}


def parse_ocr(source: Dict[str, Any]) -> Dict[str, Any]:
    path = str(source.get("source") or "")
    if not os.path.isfile(path):
        raise RuntimeError("图片不存在: %s" % path)
    tool = _which("tesseract")
    if not tool:
        raise RuntimeError("缺少依赖 tesseract；请先安装后重试")
    lang = os.environ.get("LINGOS_OCR_LANG", "chi_sim+eng")
    try:
        out = subprocess.run([tool, path, "stdout", "-l", lang], capture_output=True, timeout=120)
    except Exception as e:
        raise RuntimeError("OCR 执行失败: %s" % e)
    text = (out.stdout or b"").decode("utf-8", errors="ignore").strip()
    if not text:
        raise RuntimeError("OCR 未识别到文本")
    return {"title": "OCR: " + os.path.basename(path), "text": text, "meta": {"path": path}}


BUILTIN_PLUGINS: Dict[str, Dict[str, Any]] = {
    "webpage": {"id": "builtin.webpage", "name": "网页正文", "source_types": ["url", "html"], "parse": parse_webpage},
    "text": {"id": "builtin.text", "name": "纯文本/笔记", "source_types": ["text"], "parse": parse_text},
    "pdf": {"id": "builtin.pdf", "name": "PDF 文档", "source_types": ["pdf"], "parse": parse_pdf},
    "rss": {"id": "builtin.rss", "name": "RSS/Atom 订阅", "source_types": ["rss", "atom"], "parse": parse_rss},
    "audio": {"id": "builtin.audio", "name": "音频转写", "source_types": ["audio", "mp3", "wav", "m4a"], "parse": parse_audio},
    "ocr": {"id": "builtin.ocr", "name": "图片 OCR", "source_types": ["image", "png", "jpg", "jpeg"], "parse": parse_ocr},
}


def _infer_plugin(source: str, type_hint: str = "") -> str:
    t = (type_hint or "").strip().lower()
    if t in BUILTIN_PLUGINS:
        return t
    if t:
        return t
    s = (source or "").strip().lower()
    if s.startswith("http://") or s.startswith("https://"):
        if "/feed" in s or s.endswith(".xml") or "rss" in s or "atom" in s:
            return "rss"
        return "webpage"
    if s.startswith("/") and os.path.isfile(s):
        ext = os.path.splitext(s)[1].lstrip(".").lower()
        if ext == "pdf":
            return "pdf"
        if ext in ("mp3", "wav", "m4a", "flac", "ogg", "opus"):
            return "audio"
        if ext in ("png", "jpg", "jpeg", "webp", "bmp", "tiff"):
            return "ocr"
        if ext in ("txt", "md", "markdown", "log", "json", "csv"):
            return "text"
        return "text"
    return "text"


def load_plugins() -> Dict[str, Dict[str, Any]]:
    """内置 + 用户插件（/LINGOS/plugins/ingest/<id>/manifest.json + main.py）"""
    global _plugins
    with _lock:
        _plugins = dict(BUILTIN_PLUGINS)
        if os.path.isdir(PLUGIN_DIR):
            for name in sorted(os.listdir(PLUGIN_DIR)):
                d = os.path.join(PLUGIN_DIR, name)
                mf = os.path.join(d, "manifest.json")
                if not (os.path.isdir(d) and os.path.isfile(mf)):
                    continue
                try:
                    with open(mf, encoding="utf-8") as f:
                        man = json.load(f)
                    entry = os.path.join(d, man.get("entry", "main.py"))
                    if not os.path.isfile(entry):
                        continue
                    import importlib.util
                    spec = importlib.util.spec_from_file_location("lingos_ingest_" + name, entry)
                    mod = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(mod)  # type: ignore
                    fn = getattr(mod, "parse", None)
                    if not callable(fn):
                        continue
                    key = str(man.get("id") or name)
                    _plugins[key] = {"id": key, "name": man.get("name", name),
                                     "source_types": man.get("source_types", []),
                                     "parse": fn, "manifest": man}
                except Exception:
                    continue
        return _plugins


def plugin_list() -> List[Dict[str, Any]]:
    out = []
    for key, p in load_plugins().items():
        out.append({"id": key, "name": p.get("name", key),
                    "source_types": p.get("source_types", []),
                    "builtin": key in BUILTIN_PLUGINS})
    return out


def _chunk_text(text: str) -> List[str]:
    text = (text or "").strip()
    if not text:
        return []
    chunks: List[str] = []
    i = 0
    n = len(text)
    while i < n and len(chunks) < 400:
        end = min(n, i + CHUNK_CHARS)
        if end < n:
            cut = text.rfind("\n", i + CHUNK_CHARS // 2, end)
            if cut > i:
                end = cut
        piece = text[i:end].strip()
        if piece:
            chunks.append(piece)
        if end >= n:
            break
        i = max(i + CHUNK_CHARS - CHUNK_OVERLAP, end - CHUNK_OVERLAP)
    return chunks


def _load_index() -> Dict[str, Any]:
    try:
        with open(INDEX_PATH, encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict) and isinstance(d.get("docs"), list):
            return d
    except Exception:
        pass
    return {"version": 1, "docs": []}


def _save_index(idx: Dict[str, Any]) -> None:
    _ensure_dirs()
    tmp = INDEX_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(idx, f, ensure_ascii=False, indent=2)
    os.replace(tmp, INDEX_PATH)


def _save_job(job: Dict[str, Any]) -> None:
    _ensure_dirs()
    try:
        with open(os.path.join(JOBS_DIR, job["job_id"] + ".json"), "w", encoding="utf-8") as f:
            json.dump(job, f, ensure_ascii=False)
    except Exception:
        pass


def _run_job(job: Dict[str, Any], plugin_key: str) -> None:
    plugins = load_plugins()
    p = plugins.get(plugin_key)
    job["status"] = "running"
    job["started"] = _now()
    _save_job(job)
    try:
        if not p:
            raise RuntimeError("插件不存在: %s" % plugin_key)
        doc = p["parse"]({"source": job["source"], "text": job.get("text", ""),
                          "title": job.get("title", ""), "meta": job.get("meta", {})})
        text = str(doc.get("text") or "")[:MAX_TEXT]
        if not text.strip():
            raise RuntimeError("解析结果为空")
        chunks = _chunk_text(text)
        doc_id = _safe_id("doc")
        doc_rec = {
            "id": doc_id,
            "title": str(doc.get("title") or job["source"])[:200],
            "source": job["source"][:500],
            "plugin": plugin_key,
            "created": _now(),
            "chars": len(text),
            "chunks": len(chunks),
            "meta": doc.get("meta", {}),
            "text": text,
        }
        _ensure_dirs()
        with open(os.path.join(DOCS_DIR, doc_id + ".json"), "w", encoding="utf-8") as f:
            json.dump(doc_rec, f, ensure_ascii=False)
        with _lock:
            idx = _load_index()
            idx["docs"].insert(0, {"id": doc_id, "title": doc_rec["title"],
                                   "source": doc_rec["source"], "plugin": plugin_key,
                                   "created": doc_rec["created"], "chars": doc_rec["chars"],
                                   "chunks": len(chunks)})
            idx["docs"] = idx["docs"][:2000]
            _save_index(idx)
        job.update({"status": "done", "finished": _now(), "doc_id": doc_id,
                    "chars": len(text), "chunks": len(chunks)})
    except Exception as e:
        job.update({"status": "error", "finished": _now(), "error": str(e)[:500]})
    finally:
        with _lock:
            _jobs[job["job_id"]] = dict(job)
        _save_job(job)


def add(source: str, type_hint: str = "", title: str = "", text: str = "",
        wait: bool = False) -> Dict[str, Any]:
    src = (source or "").strip()
    txt = (text or "").strip()
    if not src and not txt:
        return {"status": "error", "msg": "source/text 为空"}
    plugin_key = _infer_plugin(src, type_hint)
    job = {"job_id": _safe_id("job"), "source": src[:1000], "text": txt[:MAX_TEXT],
           "title": title, "plugin": plugin_key, "status": "queued", "created": _now(),
           "meta": {}}
    with _lock:
        _jobs[job["job_id"]] = dict(job)
    _save_job(job)
    t = threading.Thread(target=_run_job, args=(job, plugin_key), daemon=True)
    t.start()
    if wait:
        t.join(timeout=120)
        with _lock:
            return {"status": "ok", "job": dict(_jobs.get(job["job_id"], job))}
    return {"status": "ok", "job_id": job["job_id"], "plugin": plugin_key}


def status(job_id: str = "") -> Dict[str, Any]:
    with _lock:
        if job_id:
            j = _jobs.get(job_id)
            if not j:
                try:
                    with open(os.path.join(JOBS_DIR, job_id + ".json"), encoding="utf-8") as f:
                        j = json.load(f)
                except Exception:
                    j = None
            if not j:
                return {"status": "error", "msg": "任务不存在"}
            return {"status": "ok", "job": j}
        recent = sorted(_jobs.values(), key=lambda x: x.get("created", 0), reverse=True)[:20]
        return {"status": "ok", "jobs": recent}


def list_docs(limit: int = 20) -> Dict[str, Any]:
    idx = _load_index()
    docs = idx.get("docs", [])[:max(1, min(200, int(limit or 20)))]
    return {"status": "ok", "count": len(idx.get("docs", [])), "data": docs}


def _doc_path(doc_id: str) -> str:
    return os.path.join(DOCS_DIR, str(doc_id) + ".json")


def search(query: str, limit: int = 5) -> Dict[str, Any]:
    q = (query or "").strip().lower()
    if not q:
        return {"status": "error", "msg": "query 为空"}
    idx = _load_index()
    hits: List[Dict[str, Any]] = []
    terms = [t for t in re.split(r"\s+", q) if t][:6]
    for meta in idx.get("docs", [])[:500]:
        try:
            with open(_doc_path(meta["id"]), encoding="utf-8") as f:
                doc = json.load(f)
        except Exception:
            continue
        text = str(doc.get("text") or "")
        low = text.lower()
        score = sum(low.count(t) for t in terms)
        if score <= 0 and q not in low:
            continue
        pos = low.find(terms[0]) if terms else low.find(q)
        pos = max(0, pos)
        snippet = text[max(0, pos - 60):pos + 160].replace("\n", " ")
        hits.append({"id": doc.get("id"), "title": doc.get("title"), "source": doc.get("source"),
                     "plugin": doc.get("plugin"), "score": score, "snippet": snippet})
        if len(hits) >= max(1, min(20, int(limit or 5))):
            break
    hits.sort(key=lambda x: x["score"], reverse=True)
    return {"status": "ok", "query": q, "count": len(hits), "hits": hits}


def delete(doc_id: str) -> Dict[str, Any]:
    if not doc_id:
        return {"status": "error", "msg": "doc_id 为空"}
    with _lock:
        idx = _load_index()
        before = len(idx.get("docs", []))
        idx["docs"] = [d for d in idx.get("docs", []) if d.get("id") != doc_id]
        _save_index(idx)
    p = _doc_path(doc_id)
    try:
        if os.path.isfile(p):
            os.remove(p)
    except Exception:
        pass
    return {"status": "ok", "removed": before - len(idx.get("docs", []))}


# ============================================================
# 命令面（EXT_MAP）
# ============================================================

def cmd_ingest_add(source: str = "", type: str = "", title: str = "", text: str = "") -> Dict[str, Any]:
    return add(source, type_hint=type, title=title, text=text)


def cmd_ingest_status(job_id: str = "") -> Dict[str, Any]:
    return status(job_id)


def cmd_ingest_list(limit: int = 20) -> Dict[str, Any]:
    return list_docs(limit)


def cmd_ingest_search(query: str = "", limit: int = 5) -> Dict[str, Any]:
    return search(query, limit)


def cmd_ingest_plugins() -> Dict[str, Any]:
    return {"status": "ok", "data": plugin_list()}


def cmd_ingest_delete(doc_id: str = "") -> Dict[str, Any]:
    return delete(doc_id)


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print(json.dumps({"usage": "ingest_manager.py add <src> | list | search <q> | plugins"}, ensure_ascii=False))
        sys.exit(0)
    act = sys.argv[1]
    if act == "add":
        print(json.dumps(add(sys.argv[2] if len(sys.argv) > 2 else "", wait=True), ensure_ascii=False))
    elif act == "list":
        print(json.dumps(list_docs(), ensure_ascii=False))
    elif act == "search":
        print(json.dumps(search(sys.argv[2] if len(sys.argv) > 2 else ""), ensure_ascii=False))
    elif act == "plugins":
        print(json.dumps(plugin_list(), ensure_ascii=False))
    else:
        print(json.dumps({"error": "unknown action"}, ensure_ascii=False))
