#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Memory Retrieval（记忆检索模块）
版本: LN-B-5.0.0.0-rc0.4
功能：双模式记忆检索：
  - 模式 A（语义）：通过 embed.sock 调用 sentence-transformers（若可用）
  - 模式 B（降级）：本地关键词 TF 权重检索（零依赖，跛脚）
核心协议：C1（容错/指针检查/日志）、C-C（容错编程/跛脚编程）
"""

import os
import re
import json
import sqlite3
import socket
import logging

logger = logging.getLogger("MemoryRetrieval")

# ========== 常量 ==========
VECTOR_DB = "/LINGOS/data/ai_memory/vectors.db"
EMBED_SOCKET = "/LINGOS/run/embed.sock"
EMBED_TIMEOUT = 3
MIN_SEMANTIC_SCORE = 0.15      # 语义相关度阈值（低于则视为无关）
MAX_CONTENT_LEN = 500          # 注入时内容截断长度


# =============================================================
# 数据源读取（SQLite vectors.db，只读）
# =============================================================

def _read_all_memories():
    """读取全部记忆条目（memory_id / type / content）"""
    if not os.path.exists(VECTOR_DB):
        logger.debug("Memory DB not found: %s", VECTOR_DB)
        return []
    try:
        conn = sqlite3.connect("file:%s?mode=ro" % VECTOR_DB, uri=True)
        cur = conn.cursor()
        cur.execute("SELECT memory_id, type, content FROM memory_vectors")
        rows = cur.fetchall()
        conn.close()
        return [
            {"memory_id": r[0], "type": r[1] or "long", "content": r[2]}
            for r in rows if r[2] and str(r[2]).strip()
        ]
    except Exception as e:
        logger.warning("Read memory DB failed: %s", e)
        return []


# =============================================================
# 语义模式（embed_service socket）
# =============================================================

def _get_embedding(text):
    """通过 embed.sock 获取语义向量；失败返回 None（触发降级）"""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(EMBED_TIMEOUT)
        sock.connect(EMBED_SOCKET)
        req = json.dumps({"cmd": "embed", "text": text}) + "\n"
        sock.send(req.encode())
        resp = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            resp += chunk
            if b"\n" in resp:
                break
        sock.close()
        data = json.loads(resp.decode())
        if data.get("status") == "ok":
            return data.get("embedding")
        return None
    except Exception as e:
        logger.debug("Semantic embedding unavailable: %s", e)
        return None


def _cosine_sim(a, b):
    """余弦相似度（长度不符返回 0）"""
    if not a or not b or len(a) != len(b):
        return 0.0
    dot = sum(x * y for x, y in zip(a, b))
    na = sum(x * x for x in a) ** 0.5
    nb = sum(x * x for x in b) ** 0.5
    if na == 0.0 or nb == 0.0:
        return 0.0
    return dot / (na * nb)


def _semantic_search(query, memories, top_k):
    """语义检索：query 与每条记忆实时向量化后比较（记忆量小时可行）"""
    qv = _get_embedding(query)
    if not qv:
        return None   # 语义不可用 → 触发降级

    scored = []
    for m in memories:
        mv = _get_embedding(m["content"])
        if mv:
            sim = _cosine_sim(qv, mv)
            if sim >= MIN_SEMANTIC_SCORE:
                scored.append((sim, m))
    if not scored:
        return []

    scored.sort(key=lambda x: -x[0])
    return [
        {"memory_id": m["memory_id"], "type": m["type"],
         "content": m["content"], "score": round(sim, 4)}
        for sim, m in scored[:top_k]
    ]


# =============================================================
# 关键词模式（零依赖降级）
# =============================================================

def _tokenize(text):
    """简单分词：英文单词 + 中文单字（零依赖）"""
    tokens = re.findall(r"[a-zA-Z0-9_]+|[\u4e00-\u9fff]", (text or "").lower())
    return tokens


def _keyword_search(query, memories, top_k):
    """关键词 TF 权重检索（跛脚降级）"""
    q_tokens = _tokenize(query)
    if not q_tokens:
        return []

    scored = []
    for m in memories:
        content = (m.get("content") or "").lower()
        score = 0.0
        for tok in q_tokens:
            if tok in content:
                score += 1.0
        if score > 0.0:
            scored.append((score, m))

    scored.sort(key=lambda x: -x[0])
    return [
        {"memory_id": m["memory_id"], "type": m["type"],
         "content": m["content"], "score": round(score, 4)}
        for score, m in scored[:top_k]
    ]


# =============================================================
# 对外 API
# =============================================================

def retrieve_memories(query, top_k=5, important_only=False):
    """双模式检索：优先语义，失败自动降级关键词（容错/跛脚）

    :param query: 用户请求文本
    :param top_k: 注入条数上限（默认 5，可配置）
    :param important_only: 仅检索重要记忆（[重要] 前缀——0.2.0 双记忆自动注入专用）
    :return: 记忆列表 [{memory_id, type, content, score}, ...]
    """
    if not query or not query.strip() or top_k <= 0:
        return []

    memories = _read_all_memories()
    if not memories:
        return []

    if important_only:
        memories = [m for m in memories if str(m.get("content", "")).startswith("[重要]")]

    # 模式 A：语义检索
    semantic = _semantic_search(query, memories, top_k)
    if semantic is not None:
        logger.info("Semantic retrieval: %d memories (top_k=%d, important=%s)",
                    len(semantic), top_k, important_only)
        return semantic

    # 模式 B：关键词降级
    kw = _keyword_search(query, memories, top_k)
    logger.info("Keyword retrieval fallback: %d memories (top_k=%d, important=%s)",
                len(kw), top_k, important_only)
    return kw


def search_keywords(query: str, top_k: int = 10) -> list:
    """【0.2.0 索引增强】关键词精确检索（AI 自主调用——memory_search 精确化）"""
    if not query or not query.strip():
        return []
    memories = _read_all_memories()
    if not memories:
        return []
    return _keyword_search(query, memories, top_k)


def find_important(top_k: int = 20) -> list:
    """【0.2.0 索引增强】重要记忆索引（[重要] 前缀——AI 查看常驻信息/用户偏好）"""
    memories = [m for m in _read_all_memories()
                if str(m.get("content", "")).startswith("[重要]")]
    # 按类型排序：short 优先（常驻信息）
    memories.sort(key=lambda m: 0 if m.get("type") == "short" else 1)
    return memories[:top_k]


def search_constants(keyword: str = "", top_k: int = 20) -> list:
    """【0.2.0 索引增强】常量/重要信息查找（用户偏好、固定事实等短记忆）"""
    memories = [m for m in _read_all_memories()
                if m.get("type") == "short" or str(m.get("content", "")).startswith("[重要]")]
    if keyword and keyword.strip():
        memories = [m for m in memories if keyword.lower() in str(m.get("content", "")).lower()]
    return memories[:top_k]


def build_memory_section(hits):
    """将检索结果格式化为注入文本（含截断防冲击）"""
    if not hits:
        return ""
    lines = ["## Related Memories (auto-retrieved)",
             "以下是自动检索到的相关历史记忆，可辅助回答；若需更多细节请用 memory_search 检索："]
    for h in hits:
        content = h["content"]
        if len(content) > MAX_CONTENT_LEN:
            content = content[:MAX_CONTENT_LEN] + "..."
        lines.append("- [%s] %s" % (h.get("type", "mem"), content))
    return "\n".join(lines)
