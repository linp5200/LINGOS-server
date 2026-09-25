#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS AI 能力扩展（先生 2026-09-12 裁决 · 批次4）

补齐 4 项缺失：
  M16 文档问答 RAG   —— 接线 embed_service（此前是死代码）
  M17 知识库上传     —— 文档入库 + 分块 + 索引
  M18 会话分享/导出  —— Markdown / JSON / 纯文本
  M19 图片生成       —— 复用多模态/图像模型（minis-model-use 风格：OpenAI /images 或 Gemini）

设计：纯 Python，无新增外部依赖（embed 不可用时退化为关键词检索）
"""

import json
import os
import re
import time
import hashlib
import logging
import threading
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger("AiExt")

KB_DIR = "/LINGOS/data/knowledge"
KB_INDEX = "/LINGOS/data/knowledge/index.json"
EXPORT_DIR = "/LINGOS/data/exports"
IMAGE_DIR = "/LINGOS/data/images"

_lock = threading.RLock()


# =============================================================
# 通用工具
# =============================================================
def _ensure_dirs() -> None:
    for d in (KB_DIR, EXPORT_DIR, IMAGE_DIR):
        try:
            os.makedirs(d, exist_ok=True)
        except Exception:
            pass


def _t(en: str, zh: str) -> str:
    try:
        from skill_handlers import t as _sht
        return _sht(en, zh)
    except Exception:
        return zh


def _load_json(path: str, default):
    try:
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                return json.load(f)
    except Exception as e:
        logger.debug("load %s failed: %s", path, e)
    return default


def _save_json(path: str, data) -> None:
    with _lock:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(tmp, path)


# =============================================================
# M17 · 知识库上传（文档入库 + 分块 + 索引）
# =============================================================
def _chunk_text(text: str, size: int = 800, overlap: int = 100) -> List[str]:
    """按段落/长度分块（保留语义边界优先）"""
    text = re.sub(r"\r\n", "\n", text or "")
    paras = [p.strip() for p in re.split(r"\n\s*\n", text) if p.strip()]
    chunks, cur = [], ""
    for p in paras:
        if len(cur) + len(p) + 1 <= size:
            cur = (cur + "\n" + p) if cur else p
        else:
            if cur:
                chunks.append(cur)
            if len(p) <= size:
                cur = p
            else:
                # 长段落按句切
                sents = re.split(r"(?<=[。！？.!?])\s*", p)
                cur = ""
                for s in sents:
                    if len(cur) + len(s) <= size:
                        cur += s
                    else:
                        if cur:
                            chunks.append(cur)
                        cur = s[:size]
    if cur:
        chunks.append(cur)
    # 加重叠
    if overlap > 0 and len(chunks) > 1:
        out = []
        for i, c in enumerate(chunks):
            if i > 0:
                out.append(chunks[i - 1][-overlap:] + "\n" + c)
            else:
                out.append(c)
        return out
    return chunks


def _extract_text(path: str) -> str:
    """从文件提取纯文本（支持 txt/md/json/csv/代码；其他尽力而为）"""
    ext = os.path.splitext(path)[1].lower()
    try:
        if ext in (".txt", ".md", ".markdown", ".log", ".csv", ".tsv",
                   ".json", ".yaml", ".yml", ".xml", ".html", ".htm",
                   ".py", ".c", ".h", ".js", ".dart", ".sh", ".rs", ".go", ".java"):
            with open(path, encoding="utf-8", errors="ignore") as f:
                data = f.read()
            if ext in (".html", ".htm"):
                data = re.sub(r"<script[\s\S]*?</script>", "", data)
                data = re.sub(r"<style[\s\S]*?</style>", "", data)
                data = re.sub(r"<[^>]+>", " ", data)
            return data
        # PDF/Office：尝试 pdftotext / 文本化工具（存在则用）
        import subprocess
        if ext == ".pdf":
            try:
                r = subprocess.run(["pdftotext", path, "-"], capture_output=True, timeout=30)
                if r.returncode == 0:
                    return r.stdout.decode("utf-8", "ignore")
            except Exception:
                pass
        # 最后兜底：二进制中提取可打印串
        with open(path, "rb") as f:
            raw = f.read()
        return "".join(chr(b) if 32 <= b < 127 or b in (10, 13) else " " for b in raw[:500000])
    except Exception as e:
        logger.warning("extract text failed for %s: %s", path, e)
        return ""


def _embed_texts(texts: List[str]) -> Optional[List[List[float]]]:
    """调用 embed_service（若可用）；失败返回 None（上层退化为关键词检索）"""
    try:
        import embed_service as ES
    except Exception as e:
        logger.debug("embed_service unavailable: %s", e)
        return None
    vecs = []
    try:
        for t in texts:
            v = None
            for fn in ("embed_text", "encode", "embed"):
                if hasattr(ES, fn):
                    try:
                        v = getattr(ES, fn)(t)
                        break
                    except Exception:
                        continue
            if v is None:
                return None
            if hasattr(v, "tolist"):
                v = v.tolist()
            vecs.append(list(v))
        return vecs if vecs else None
    except Exception as e:
        logger.warning("embed failed: %s", e)
        return None


def cmd_kb_upload(path: str = "", name: str = "", tags: str = "") -> dict:
    """上传文档到知识库（分块 + 索引 + 可选向量）"""
    _ensure_dirs()
    if not path or not os.path.exists(path):
        return {"status": "error", "msg": _t("File not found", "文件不存在")}

    text = _extract_text(path)
    if not text.strip():
        return {"status": "error", "msg": _t("No extractable text", "无可提取文本")}

    doc_id = hashlib.sha256((path + str(time.time())).encode()).hexdigest()[:12]   # 【0.7.0-hf2】sha1→sha256（bandit B324）
    chunks = _chunk_text(text)
    vectors = _embed_texts(chunks)

    doc = {
        "id": doc_id,
        "name": name or os.path.basename(path),
        "source": path,
        "size": os.path.getsize(path),
        "chars": len(text),
        "chunks": len(chunks),
        "tags": [x.strip() for x in (tags or "").split(",") if x.strip()],
        "created": int(time.time()),
        "has_vectors": bool(vectors),
    }

    # 落盘分块
    cpath = os.path.join(KB_DIR, doc_id + ".json")
    _save_json(cpath, {"doc": doc, "chunks": chunks, "vectors": vectors})

    # 更新索引
    idx = _load_json(KB_INDEX, {"docs": [], "total_chunks": 0})
    idx["docs"] = [d for d in idx.get("docs", []) if d.get("id") != doc_id]
    idx["docs"].append(doc)
    idx["total_chunks"] = sum(d.get("chunks", 0) for d in idx["docs"])
    _save_json(KB_INDEX, idx)

    logger.info("KB uploaded: %s (%d chunks, vectors=%s)", doc["name"], len(chunks), bool(vectors))
    return {"status": "ok", "data": doc}


def cmd_kb_list() -> dict:
    idx = _load_json(KB_INDEX, {"docs": [], "total_chunks": 0})
    return {"status": "ok", "data": idx}


def cmd_kb_remove(doc_id: str = "") -> dict:
    if not doc_id:
        return {"status": "error", "msg": "缺少 doc_id"}
    try:
        p = os.path.join(KB_DIR, doc_id + ".json")
        if os.path.exists(p):
            os.remove(p)
    except Exception as e:
        logger.debug("remove kb file failed: %s", e)
    idx = _load_json(KB_INDEX, {"docs": [], "total_chunks": 0})
    idx["docs"] = [d for d in idx.get("docs", []) if d.get("id") != doc_id]
    idx["total_chunks"] = sum(d.get("chunks", 0) for d in idx["docs"])
    _save_json(KB_INDEX, idx)
    return {"status": "ok", "data": {"removed": doc_id}}


# =============================================================
# M16 · 文档问答 RAG
# =============================================================
def _cosine(a: List[float], b: List[float]) -> float:
    try:
        s = sum(x * y for x, y in zip(a, b))
        na = sum(x * x for x in a) ** 0.5
        nb = sum(y * y for y in b) ** 0.5
        return s / (na * nb) if na and nb else 0.0
    except Exception:
        return 0.0


def _tokens(s: str) -> set:
    """分词（英文按词 + 中文按 bigram —— 无需 jieba 依赖）"""
    s = (s or "").lower()
    en = set(re.findall(r"[a-z0-9_]+", s))
    zh_chars = re.findall(r"[\u4e00-\u9fff]", s)
    zh = set()
    for i in range(len(zh_chars)):
        zh.add(zh_chars[i])
        if i + 1 < len(zh_chars):
            zh.add(zh_chars[i] + zh_chars[i + 1])
    return en | zh


def _keyword_score(query: str, text: str) -> float:
    """退化的关键词评分（无向量时使用；支持中英文）"""
    q = _tokens(query)
    if not q:
        return 0.0
    t = _tokens(text)
    if not t:
        return 0.0
    return len(q & t) / len(q)


def cmd_kb_search(query: str = "", top_k: int = 5,
                  doc_id: str = "") -> dict:
    """检索知识库（向量优先；无向量退化为关键词）"""
    if not query:
        return {"status": "error", "msg": "缺少 query"}

    idx = _load_json(KB_INDEX, {"docs": []})
    docs = [d for d in idx.get("docs", []) if (not doc_id or d.get("id") == doc_id)]

    # 查询向量
    qvec = None
    vecs = _embed_texts([query])
    if vecs:
        qvec = vecs[0]

    scored = []
    for d in docs:
        data = _load_json(os.path.join(KB_DIR, d.get("id", "") + ".json"), None)
        if not data:
            continue
        chunks = data.get("chunks", [])
        cvecs = data.get("vectors")
        for i, c in enumerate(chunks):
            if qvec and cvecs and i < len(cvecs) and cvecs[i]:
                score = _cosine(qvec, cvecs[i])
                method = "vector"
            else:
                score = _keyword_score(query, c)
                method = "keyword"
            if score > 0:
                scored.append({"doc_id": d.get("id"), "doc": d.get("name"),
                               "chunk_index": i, "score": round(score, 4),
                               "method": method, "text": c[:600]})
    scored.sort(key=lambda x: -x["score"])
    top = scored[:max(1, min(top_k, 20))]
    return {"status": "ok", "data": {
        "query": query, "hits": len(top),
        "results": top,
        "mode": "vector" if qvec else "keyword",
    }}


def cmd_kb_ask(query: str = "", top_k: int = 4) -> dict:
    """RAG 问答：检索 → 拼上下文 → 交 LLM

    返回 retrieved（检索片段）+ prompt（供上层调用 LLM 的完整提示）
    """
    r = cmd_kb_search(query, top_k)
    if r.get("status") != "ok":
        return r
    hits = r["data"]["results"]
    if not hits:
        return {"status": "ok", "data": {"answer": None,
                                          "note": _t("No relevant knowledge found",
                                                     "知识库中未找到相关内容"),
                                          "retrieved": []}}
    ctx_parts = []
    for i, h in enumerate(hits, 1):
        ctx_parts.append("[%d] （来自《%s》）\n%s" % (i, h.get("doc"), h.get("text")))
    context = "\n\n".join(ctx_parts)
    prompt = (
        "请**仅依据**以下资料回答问题；资料不足时明确说明「资料中未提及」。\n\n"
        "=== 资料 ===\n" + context + "\n=== 资料结束 ===\n\n"
        "问题：" + query
    )
    return {"status": "ok", "data": {
        "retrieved": hits,
        "context": context,
        "prompt": prompt,
        "mode": r["data"]["mode"],
        "note": _t("Pass 'prompt' to the LLM for the final answer",
                   "将 prompt 交给 LLM 生成最终答案"),
    }}


# =============================================================
# M18 · 会话分享 / 导出
# =============================================================
def _fmt_ts(ts) -> str:
    try:
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(ts)))
    except Exception:
        return ""


def cmd_session_export(session_id: str = "", fmt: str = "markdown",
                       messages: str = "", title: str = "") -> dict:
    """导出会话为文件。messages 为 JSON 数组（[{role, content, ts}]）或留空由调用方提供"""
    _ensure_dirs()
    try:
        msgs = json.loads(messages) if messages else []
    except Exception:
        msgs = []
    if not isinstance(msgs, list):
        msgs = []

    fmt = (fmt or "markdown").lower()
    if fmt in ("md", "markdown"):
        ext, mime = "md", "text/markdown"
    elif fmt == "json":
        ext, mime = "json", "application/json"
    elif fmt in ("txt", "text"):
        ext, mime = "txt", "text/plain"
    else:
        ext, mime = "md", "text/markdown"

    sid = session_id or "session"
    fname = "%s_%d.%s" % (re.sub(r"[^\w\-]", "_", sid), int(time.time()), ext)
    path = os.path.join(EXPORT_DIR, fname)

    if ext == "json":
        content = json.dumps({"session": sid, "title": title, "exported": int(time.time()),
                              "messages": msgs}, ensure_ascii=False, indent=2)
    else:
        head = "# %s\n\n> 会话 `%s` · 导出 %s · 共 %d 条\n\n---\n\n" % (
            title or sid, sid, _fmt_ts(time.time()), len(msgs))
        lines = []
        for m in msgs:
            role = (m.get("role") or "?").lower()
            who = {"user": "🧑 用户", "assistant": "🤖 Nook", "system": "⚙️ 系统",
                   "tool": "🔧 工具"}.get(role, role)
            ts = _fmt_ts(m.get("ts") or m.get("timestamp") or 0)
            body = str(m.get("content") or "")
            if ext == "txt":
                lines.append("[%s] %s\n%s\n" % (ts, who, body))
            else:
                lines.append("### %s  <sub>%s</sub>\n\n%s\n" % (who, ts, body))
        content = head + "\n".join(lines)

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

    return {"status": "ok", "data": {
        "path": path, "filename": fname, "format": ext, "mime": mime,
        "size": len(content.encode("utf-8")),
        "messages": len(msgs),
    }}


def cmd_session_share_text(session_id: str = "", messages: str = "",
                           title: str = "", max_chars: int = 20000) -> dict:
    """生成「可分享文本」（用于剪贴板/分享面板），截断到上限"""
    try:
        msgs = json.loads(messages) if messages else []
    except Exception:
        msgs = []
    head = "【%s】会话导出\n\n" % (title or session_id or "LING OS")
    parts = [head]
    total = len(head)
    for m in msgs:
        role = (m.get("role") or "?").lower()
        who = {"user": "用户", "assistant": "Nook", "system": "系统",
               "tool": "工具"}.get(role, role)
        seg = "%s：%s\n\n" % (who, str(m.get("content") or "")[:2000])
        if total + len(seg) > max_chars:
            parts.append("…（已截断，完整内容请导出文件）\n")
            break
        parts.append(seg)
        total += len(seg)
    text = "".join(parts)
    return {"status": "ok", "data": {"text": text, "chars": len(text),
                                     "truncated": total >= max_chars}}


# =============================================================
# M19 · 图片生成
# =============================================================
def cmd_image_generate(prompt: str = "", model: str = "", size: str = "1024x1024",
                       n: int = 1, provider: str = "") -> dict:
    """图片生成（走已配置的图像模型）

    优先使用 minis-model-use 风格的统一调用（若环境提供）；
    否则返回**明确的配置指引**（不伪造结果）。
    """
    _ensure_dirs()
    if not prompt:
        return {"status": "error", "msg": "缺少 prompt"}
    n = max(1, min(4, int(n)))

    # ① 若上层提供图像生成能力，优先使用
    for modname in ("image_service", "llm_unified"):
        try:
            mod = __import__(modname)
        except Exception:
            continue
        for fn in ("generate_image", "image_generate", "create_image"):
            if hasattr(mod, fn):
                try:
                    res = getattr(mod, fn)(prompt, size=size, n=n, model=model)
                    return {"status": "ok", "data": res} if isinstance(res, dict) else \
                           {"status": "ok", "data": {"result": res}}
                except Exception as e:
                    logger.warning("%s.%s failed: %s", modname, fn, e)

    # ② 未配置 → 明确告知（不伪造）
    cfg_hint = {
        "needs_config": True,
        "how_to": _t(
            "Image generation requires a provider with image_output capability. "
            "Add one in Settings → Model Providers (or AI Config → 提供商), "
            "then set it as the active image model.",
            "图片生成需要一个具备 image_output 能力的提供商。"
            "请在「设置 → 模型与提供商」中添加，并设为当前图像模型。"),
        "where": "/LINGOS/system/config/provider.json",
    }
    logger.warning("image_generate: no image-capable provider configured")
    return {"status": "error", "error_type": "not_configured",
            "msg": cfg_hint["how_to"], "data": cfg_hint}


def cmd_image_list(limit: int = 50) -> dict:
    _ensure_dirs()
    try:
        files = []
        for f in os.listdir(IMAGE_DIR):
            p = os.path.join(IMAGE_DIR, f)
            if os.path.isfile(p):
                files.append({"name": f, "size": os.path.getsize(p),
                              "mtime": int(os.path.getmtime(p))})
        files.sort(key=lambda x: -x["mtime"])
        return {"status": "ok", "data": files[:max(1, min(limit, 500))]}
    except Exception as e:
        return {"status": "ok", "data": [], "note": str(e)}


def cmd_kb_ask(query: str = "", top_k: int = 5, doc_id: str = "") -> dict:
    """RAG 问答：检索 → 组织上下文（供上层调用 LLM）"""
    r = cmd_kb_search(query, top_k, doc_id)
    if r.get("status") != "ok":
        return r
    hits = r["data"].get("results", [])
    ctx = "\n\n".join(
        "【片段 %d｜来自 %s】\n%s" % (i + 1, h.get("doc"), h.get("text"))
        for i, h in enumerate(hits))
    prompt = (
        "请依据下列资料回答问题；资料中未提及的内容请明确说明「资料未涵盖」。\n\n"
        "=== 资料 ===\n%s\n=== 资料结束 ===\n\n问：%s" % (ctx, query)
    ) if hits else ""
    return {"status": "ok", "data": {
        "query": query, "hits": hits, "context": ctx, "prompt": prompt,
        "answer": None,
        "note": _t("Pass 'prompt' to the LLM to obtain the final answer",
                   "将 prompt 交给 LLM 即可得到最终答案"),
    }}
