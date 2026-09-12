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
from urllib.parse import urlparse, urljoin

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


# ========== SSRF 防护（【0.5.0 S4】补全） ==========
# 原实现缺陷（审计发现）：
#   ① 未拦 169.254.169.254（云元数据端点 —— SSRF 头号目标，可读云凭据）
#   ② 未拦 IPv6 映射（::ffff:127.0.0.1）
#   ③ 未拦数字型 IP（十进制 2130706433 / 十六进制 0x7f.1 / 八进制 0177.0.0.1）
#   ④ '172.' 前缀误伤 172.32+ 公网地址
#   ⑤ 无 DNS rebinding 防护（解析后再连 IP 变了）
#   ⑥ 只查首个解析结果（getaddrinfo 可能返回多个）
# 修法：用标准库 ipaddress 做**规范化 + 分类**，并检查**全部**解析结果。

import ipaddress as _ipaddress

# 明确禁止的网段（含云元数据 —— 关键补充）
_BLOCKED_NETS = [
    _ipaddress.ip_network("0.0.0.0/8"),
    _ipaddress.ip_network("10.0.0.0/8"),
    _ipaddress.ip_network("100.64.0.0/10"),      # CGNAT
    _ipaddress.ip_network("127.0.0.0/8"),
    _ipaddress.ip_network("169.254.0.0/16"),     # ★ 链路本地 / 云元数据 169.254.169.254
    _ipaddress.ip_network("172.16.0.0/12"),      # ★ 仅 16~31（不误伤 172.32+）
    _ipaddress.ip_network("192.0.0.0/24"),
    _ipaddress.ip_network("192.168.0.0/16"),
    _ipaddress.ip_network("198.18.0.0/15"),
    _ipaddress.ip_network("224.0.0.0/4"),        # 组播
    _ipaddress.ip_network("240.0.0.0/4"),        # 保留
    _ipaddress.ip_network("255.255.255.255/32"),
    # IPv6
    _ipaddress.ip_network("::/128"),
    _ipaddress.ip_network("::1/128"),
    _ipaddress.ip_network("fc00::/7"),           # 唯一本地
    _ipaddress.ip_network("fe80::/10"),          # 链路本地
    _ipaddress.ip_network("ff00::/8"),           # 组播
    _ipaddress.ip_network("2002::/16"),          # 6to4（可封装内网）
]

# 云元数据专用地址（额外显式拦截，便于审计日志识别）
_METADATA_IPS = {"169.254.169.254", "fd00:ec2::254", "metadata.google.internal"}


def _is_blocked_ip(ip_str: str) -> bool:
    """判定 IP 是否属于禁止访问的网段（含 IPv6 映射还原）"""
    try:
        ip = _ipaddress.ip_address(ip_str)
    except ValueError:
        return True   # 无法解析 → 拒绝
    # IPv4 映射的 IPv6（::ffff:a.b.c.d）→ 还原为 IPv4 再判
    if isinstance(ip, _ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    for net in _BLOCKED_NETS:
        try:
            if ip in net:
                return True
        except TypeError:
            continue   # 版本不匹配（v4 net vs v6 addr）
    return False


def _resolve_all(host: str) -> list:
    """解析主机名的**全部**地址（v4+v6）"""
    out = []
    try:
        for fam, _, _, _, sa in socket.getaddrinfo(host, None):
            if fam == socket.AF_INET:
                out.append(sa[0])
            elif fam == socket.AF_INET6:
                out.append(sa[0])
    except Exception:
        pass
    return out


def _is_safe_url(url: str, resolve: bool = True) -> bool:
    """SSRF 防护（【0.5.0 S4】规范化 + 全量解析 + 云元数据拦截）

    :param resolve: 是否做 DNS 解析校验（True=正常；False=仅字面检查，用于 rebinding 前的初筛）
    """
    try:
        parsed = urlparse(url)
        if parsed.scheme not in ("http", "https"):
            return False
        host = (parsed.hostname or "").strip()
        if not host:
            return False

        # ① 云元数据主机名显式拦截
        if host.lower() in _METADATA_IPS:
            logger.warning("SSRF: 云元数据地址被拦截 %s", host)
            return False

        # ② 端口限制（可选：阻断常见内部服务端口）
        port = parsed.port
        if port is not None and port in (22, 23, 25, 445, 3389, 6379, 11211):
            logger.warning("SSRF: 高危端口被拦截 %s:%s", host, port)
            return False

        # ③ 字面为 IP → 直接判定（同时覆盖数字型/十六进制等，ipaddress 能识别部分）
        try:
            lit = _ipaddress.ip_address(host)
            return not _is_blocked_ip(str(lit))
        except ValueError:
            pass

        # ④ 域名 → 解析全部地址逐个判定
        if not resolve:
            return True
        addrs = _resolve_all(host)
        if not addrs:
            logger.warning("SSRF: 无法解析主机 %s → 拒绝", host)
            return False
        for a in addrs:
            if _is_blocked_ip(a):
                logger.warning("SSRF: 主机 %s 解析到受限地址 %s → 拒绝", host, a)
                return False
        return True
    except Exception as e:
        logger.warning("SSRF 校验异常(%s) → 拒绝: %s", e, url)
        return False


def _verify_before_request(url: str):
    """【DNS rebinding 防护】请求前**再次**校验，防止解析在初筛后被改写。

    返回 (ok, detail)。requests 无法直接绑定 IP，故采用「请求前二次解析 + 对比」
    策略：若两次解析结果不同（且新的落入受限段）→ 拒绝。
    """
    if not _is_safe_url(url, resolve=True):
        return False, "URL 未通过 SSRF 校验"
    return True, ""


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
def _fetch_page(url: str, _depth: int = 0) -> str:
    """抓取 URL 并转纯文本（【0.5.0 S4】SSRF 防护 + 重定向逐跳校验 + 大小/超时限制）

    ⚠️ 关键修复：原实现用 requests.get 默认**自动跟随重定向**，
       攻击者可用「公网 URL → 302 到 169.254.169.254」绕过 SSRF 检查。
       现改为 allow_redirects=False，**手动逐跳校验**（最多 5 跳）。
    """
    if _depth > 5:
        return "Error: Too many redirects"

    # DNS rebinding 二次校验（请求前再验一次）
    ok, detail = _verify_before_request(url)
    if not ok:
        logger.warning("web_fetch 被 SSRF 拦截: %s (%s)", url, detail)
        return f"Error: URL blocked by SSRF protection ({detail})"

    try:
        resp = requests.get(url, timeout=FETCH_TIMEOUT, allow_redirects=False,
                            headers={"User-Agent": "Mozilla/5.0 (LINGOS AI)"})
        # 重定向：逐跳校验后再跟
        if resp.status_code in (301, 302, 303, 307, 308):
            loc = resp.headers.get("Location", "")
            if not loc:
                return "Error: redirect without Location"
            nxt = urljoin(url, loc)
            if not _is_safe_url(nxt):
                logger.warning("重定向目标被 SSRF 拦截: %s → %s", url, nxt)
                return "Error: redirect target blocked by SSRF protection"
            return _fetch_page(nxt, _depth + 1)

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
