#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS MCP 客户端（【0.6.1】MCP 工具接线——修复「注册的服务器无法被 AI 调用」）

协议：JSON-RPC 2.0 over HTTP（MCP Streamable HTTP 传输，2024-11 规范）
  ① initialize（协议握手——记录 Mcp-Session-Id）
  ② tools/list（工具发现）
  ③ tools/call（工具执行）

对接点：
  · ai_server.load_skill_schemas → mcp_tool_schemas() 合并进 AI 工具表
    （命名 mcp__<server>__<tool>）
  · ai_server 工具执行 → is_mcp_tool / call_mcp_tool 路由
"""

import json
import logging
import urllib.request
import urllib.error

logger = logging.getLogger("MCP")

_TIMEOUT = 10
_sessions = {}   # url → Mcp-Session-Id（initialize 返回）


def _rpc(url: str, method: str, params: dict = None, session: str = "",
         req_id: int = 1) -> dict:
    """发送一次 JSON-RPC 请求 → 返回 result dict；失败抛异常"""
    body = {
        "jsonrpc": "2.0",
        "id": req_id,
        "method": method,
    }
    if params is not None:
        body["params"] = params
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    req.add_header("User-Agent", "LINGOS-MCP/0.6.1")
    if session:
        req.add_header("Mcp-Session-Id", session)
    try:
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as r:
            sid = r.headers.get("Mcp-Session-Id", "")
            if sid:
                _sessions[url] = sid
            raw = r.read().decode("utf-8", "ignore")
    except urllib.error.HTTPError as e:
        raise RuntimeError("HTTP %s: %s" % (e.code, e.read().decode("utf-8", "ignore")[:200]))

    # SSE 响应（text/event-stream）：提取 data: 行
    if raw.lstrip().startswith("event:") or "data:" in raw[:64]:
        for line in raw.splitlines():
            if line.startswith("data:"):
                raw = line[5:].strip()
                break
    resp = json.loads(raw)
    if "error" in resp:
        raise RuntimeError("RPC error: %s" % resp["error"])
    return resp.get("result", {})


def mcp_initialize(url: str) -> dict:
    """协议握手（记录会话）"""
    result = _rpc(url, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "lingos", "version": "0.6.1"},
    })
    # 通知 initialized（部分服务器要求）
    try:
        _rpc(url, "notifications/initialized", None, _sessions.get(url, ""), req_id=2)
    except Exception:
        pass
    return result


def mcp_tools_list(url: str) -> list:
    """发现工具 → [{name, description, inputSchema}]"""
    sid = _sessions.get(url, "")
    if not sid:
        try:
            mcp_initialize(url)
            sid = _sessions.get(url, "")
        except Exception as e:
            logger.warning("mcp initialize failed for %s: %s", url, e)
    result = _rpc(url, "tools/list", {}, sid, req_id=3)
    tools = result.get("tools", [])
    return tools if isinstance(tools, list) else []


def mcp_tool_call(url: str, tool_name: str, arguments: dict) -> dict:
    """执行工具 → {"content": [...]}"""
    sid = _sessions.get(url, "")
    if not sid:
        try:
            mcp_initialize(url)
            sid = _sessions.get(url, "")
        except Exception as e:
            return {"error": "initialize failed: %s" % e}
    try:
        return _rpc(url, "tools/call", {"name": tool_name, "arguments": arguments or {}}, sid, req_id=4)
    except Exception as e:
        return {"error": str(e)}


# =============================================================
# 与 ai_server 对接的高层接口
# =============================================================

def load_mcp_config() -> dict:
    """读 mcp_servers.json（与 ai_server 同路径）"""
    import os
    path = "/LINGOS/state/mcp_servers.json"
    try:
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f) or {}
    except Exception as e:
        logger.debug("mcp config read failed: %s", e)
    return {}


def mcp_tool_schemas() -> list:
    """全部 MCP 工具 → AI 工具 schema（OpenAI function 格式）

    命名：mcp__<server>__<tool>（双下划线——防与服务端技能冲突）
    失败静默（单服务器挂不影响其他）
    """
    schemas = []
    cfg = load_mcp_config()
    for server, info in cfg.items():
        url = (info or {}).get("url", "")
        if not url:
            continue
        try:
            tools = mcp_tools_list(url)
        except Exception as e:
            logger.warning("mcp tools/list failed (%s): %s", server, e)
            continue
        for t in tools:
            if not isinstance(t, dict) or not t.get("name"):
                continue
            tname = str(t["name"])
            schema = t.get("inputSchema") or {"type": "object", "properties": {}}
            schemas.append({
                "type": "function",
                "function": {
                    "name": "mcp__%s__%s" % (server, tname),
                    "description": "[MCP:%s] %s" % (server, str(t.get("description", ""))[:200]),
                    "parameters": schema,
                    "risk": "medium",
                    "mcp": True,
                },
            })
    if schemas:
        logger.info("mcp: %d tools discovered from %d servers", len(schemas), len(cfg))
    return schemas


def is_mcp_tool(name: str) -> bool:
    return isinstance(name, str) and name.startswith("mcp__")


def call_mcp_tool(full_name: str, arguments: dict):
    """执行 MCP 工具（full_name = mcp__<server>__<tool>）→ (ok, text)"""
    parts = full_name.split("__", 2)
    if len(parts) != 3:
        return False, "MCP 工具名格式错误: %s" % full_name
    _, server, tool = parts
    cfg = load_mcp_config()
    info = cfg.get(server) or {}
    url = info.get("url", "")
    if not url:
        return False, "MCP 服务器未配置: %s" % server
    result = mcp_tool_call(url, tool, arguments or {})
    if isinstance(result, dict) and result.get("error"):
        return False, "MCP 调用失败: %s" % result["error"]
    # 结果归一化：content 数组 → 文本
    try:
        content = result.get("content", [])
        texts = []
        for item in content:
            if isinstance(item, dict):
                if item.get("type") == "text":
                    texts.append(str(item.get("text", "")))
                else:
                    texts.append(json.dumps(item, ensure_ascii=False))
            else:
                texts.append(str(item))
        text = "\n".join(texts) if texts else json.dumps(result, ensure_ascii=False)[:2000]
        return True, text[:4000]
    except Exception as e:
        return True, json.dumps(result, ensure_ascii=False)[:2000]
