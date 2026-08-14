#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Web Search（AI 联网搜索能力）
版本: LN-B-5.0.0.0-rc0.4
功能：web_search（searxng 主 + html 降级）、web_fetch（SSRF 防护）
  - 并行多主题搜索（最多 50 URL，10 次/分钟限制）
核心协议：C1（容错/日志）、C-C（容错/跛脚）、安全冗余（SSRF 防护）
"""

import os
import re
import time
import json
import socket
import logging
import threading
from urllib.parse import urlparse

import requests

logger = logging.getLogger("WebSearch")

SEARXNG_URL = os.environ.get("LINGOS_SEARXNG_URL", "http://127.0.0.1:8888")
SEARCH_TIMEOUT = 5
FETCH_TIMEOUT = 5
MAX_FETCH_SIZE = 50 * 1024      # 抓取内容上限 50KB
MAX_SEARCH_URLS = 50            # 单次搜索最多 URL
RATE_LIMIT_PER_MIN = 10         # 频率限制 次/分钟

# ========== 频率限制（滑动窗口） ==========
_rate_lock = threading.Lock()
_rate_timestamps = []


def _check_rate_limit(limit_per_min: int = RATE_LIMIT_PER_MIN) -> bool:
    """频率限制检查（滑动窗口）"""
    global _rate_timestamps
    now = time.time()
    with _rate_lock:
        _rate_timestamps = [t for t in _rate_timestamps if now - t < 60]
        if len(_rate_timestamps) >= limit_per_min:
            return False
        _rate_timestamps.append(now)
        return True


# ========== SSRF 防护 ==========
def _is_safe_url(url: str) -> bool:
    """拒绝内网/本机/非 http(s) 地址（SSRF 防护）"""
    try:
        parsed = urlparse(url)
        if parsed.scheme not in ("http", "https"):
            return False
        host = parsed.hostname or ""
        if host in ("localhost", "127.0.0.1", "::1", "0.0.0.0"):
            return False
        # 内网段
        if host.startswith("10.") or host.startswith("192.168.") or host.startswith("172."):
            return False
        # IP 解析检查
        try:
            ip = socket.gethostbyname(host)
            if ip.startswith(("10.", "192.168.", "172.")) or ip == "127.0.0.1":
                return False
        except Exception:
            return False
        return True
    except Exception:
        return False


# ========== searxng 后端 ==========
def _search_searxng(query: str, num: int) -> list:
    """searxng 自托管搜索（JSON 格式）"""
    try:
        resp = requests.get(
            SEARXNG_URL + "/search",
            params={"q": query, "format": "json"},
            timeout=SEARCH_TIMEOUT
        )
        if resp.status_code != 200:
            logger.warning("searxng returned HTTP %d", resp.status_code)
            return []
        data = resp.json()
        results = []
        for item in data.get("results", [])[:num]:
            url = item.get("url", "")
            if not url or not _is_safe_url(url):
                continue
            results.append({
                "title": item.get("title", ""),
                "url": url,
                "content": item.get("content", "")[:500]
            })
        return results
    except Exception as e:
        logger.warning("searxng search failed: %s", e)
        return []


# ========== html 降级后端（DuckDuckGo） ==========
def _search_html(query: str, num: int) -> list:
    """免费 HTML 解析搜索（降级后端）"""
    try:
        resp = requests.get(
            "https://html.duckduckgo.com/html/",
            params={"q": query},
            headers={"User-Agent": "Mozilla/5.0 (LINGOS AI)"},
            timeout=SEARCH_TIMEOUT
        )
        if resp.status_code != 200:
            return []
        # 简单解析 result__a / result__snippet
        results = []
        pattern = re.compile(
            r'<a[^>]*class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>'
            r'.*?class="result__snippet"[^>]*>(.*?)</a>', re.S)
        for m in pattern.finditer(resp.text):
            url = m.group(1)
            # DuckDuckGo 重定向链接
            if url.startswith("//duckduckgo.com/l/?uddg="):
                import urllib.parse as up
                url = up.unquote(url.split("uddg=")[1].split("&")[0])
            if not url.startswith(("http://", "https://")):
                continue
            if not _is_safe_url(url):
                continue
            title = re.sub(r"<[^>]+>", "", m.group(2)).strip()
            snippet = re.sub(r"<[^>]+>", "", m.group(3)).strip()
            results.append({"title": title, "url": url, "content": snippet[:500]})
            if len(results) >= num:
                break
        return results
    except Exception as e:
        logger.warning("html search failed: %s", e)
        return []


# ========== 抓取网页（web_fetch） ==========
def _fetch_page(url: str) -> str:
    """抓取 URL 并转纯文本（SSRF 防护 + 大小限制 + 超时）"""
    if not _is_safe_url(url):
        return "Error: URL blocked by SSRF protection (internal/private address)"
    try:
        resp = requests.get(url, timeout=FETCH_TIMEOUT,
                            headers={"User-Agent": "Mozilla/5.0 (LINGOS AI)"})
        if resp.status_code != 200:
            return f"Error: HTTP {resp.status_code}"
        # 转纯文本
        html = resp.text[:MAX_FETCH_SIZE]
        text = re.sub(r"<script[^>]*>.*?</script>", "", html, flags=re.S)
        text = re.sub(r"<style[^>]*>.*?</style>", "", text, flags=re.S)
        text = re.sub(r"<[^>]+>", " ", text)
        text = re.sub(r"\s+", " ", text).strip()
        return text[:MAX_FETCH_SIZE]
    except Exception as e:
        return f"Error: {e}"


# ========== 对外 API ==========

def web_search(query: str, num_results: int = 5, backend: str = "searxng",
               rate_limit: int = RATE_LIMIT_PER_MIN) -> list:
    """搜索网页（searxng 主 + html 降级 + 频率限制）

    :param query: 搜索词
    :param num_results: 返回条数（≤50）
    :param backend: "searxng" / "html"
    :param rate_limit: 频率限制 次/分钟
    :return: [{title, url, content}, ...]
    """
    if not query or not query.strip():
        return []
    num_results = max(1, min(num_results, MAX_SEARCH_URLS))
    if not _check_rate_limit(rate_limit):
        return [{"title": "Rate limit exceeded", "url": "", "content": ""}]

    if backend == "searxng":
        results = _search_searxng(query, num_results)
        if results:
            return results
        # searxng 不可用 → html 降级（跛脚）
        logger.info("searxng unavailable, falling back to html")
    return _search_html(query, num_results)


def web_search_multi(queries, num_per_query: int = 5, backend: str = "searxng",
                     max_urls: int = MAX_SEARCH_URLS) -> list:
    """并行搜索多个主题（合并去重）

    :param queries: 搜索词列表
    :param num_per_query: 每主题条数
    :param backend: 后端
    :param max_urls: 总 URL 上限（默认 50）
    :return: [{title, url, content}, ...]
    """
    if not queries:
        return []
    queries = [q for q in queries if q and q.strip()][:10]  # 最多 10 主题
    results = []
    seen = set()

    def _search_one(q):
        for r in web_search(q, num_per_query, backend):
            if r.get("url") and r["url"] not in seen:
                seen.add(r["url"])
                results.append(r)

    threads = [threading.Thread(target=_search_one, args=(q,)) for q in queries]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=SEARCH_TIMEOUT + 2)

    return results[:max_urls]


def web_fetch(url: str) -> str:
    """抓取网页内容（供 AI 阅读）"""
    return _fetch_page(url)
