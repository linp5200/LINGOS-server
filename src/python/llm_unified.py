#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS 统一 LLM 调用层（0.2.0——AI-AGENT#7 定稿）
- 原生直连不做转换：用哪个模型调用哪个模型的格式（不建 OpenAI⇄Anthropic 转换层）
- format: openai    → POST {base}/chat/completions（DeepSeek/Kimi/GLM/通义/Compatible/Ollama）
- format: anthropic → POST {base}/v1/messages（Anthropic 原生 Messages API）
- 对外事件统一（thinking/content/tool_calls/usage）；对内各 adapter 原生解析
- 错误分类（HTTP 400/401/402/403/404/429/5xx/超时/网络）+ 建议动作
- 429/5xx 指数退避重试 1 次
- 格式错误调试落盘 /LINGOS/logs/api_debug/（API Key 脱敏）
- 消息规范化（修复工具调用后续轮 HTTP 400 根因：tool content 非空 / assistant 带 tool_calls 时 content=""）
- 上下文窗口：显式配置 → 内置映射表 → /v1/models 查询 → None=无限
"""

import json
import os
import time
import logging
from datetime import datetime
from typing import Dict, List, Optional, Generator, Tuple

logger = logging.getLogger("LLMUnified")

PROVIDER_FILE = "/LINGOS/system/config/provider.json"
DEBUG_DIR = "/LINGOS/logs/api_debug"

# 内置上下文窗口映射表（token）——模型名小写匹配，/v1/models 获取失败时兜底
CONTEXT_WINDOW_MAP = {
    "deepseek-v4-pro": 131072, "deepseek-v4-flash": 131072,
    "deepseek-chat": 65536, "deepseek-reasoner": 65536,
    "gpt-4o": 128000, "gpt-4o-mini": 128000, "gpt-4.1": 1047576, "gpt-4.1-mini": 1047576,
    "gpt-oss:20b": 131072, "gpt-oss:120b": 131072,
    "kimi-k2": 131072, "moonshot-v1-8k": 8192, "moonshot-v1-32k": 32768, "moonshot-v1-128k": 131072,
    "glm-4.6:cloud": 131072, "glm-4.5": 131072, "glm-4": 131072,
    "qwen-max": 32768, "qwen-plus": 131072, "qwen-turbo": 131072,
    "claude-opus-4": 200000, "claude-opus-4-1": 200000, "claude-sonnet-4": 200000,
    "claude-sonnet-4-5": 200000, "claude-3-5-sonnet": 200000, "claude-3-7-sonnet": 200000,
    "claude-3-haiku": 200000, "claude-3-5-haiku": 200000,
}

# 提示词/SSE 事件等常量
MAX_TOKENS_DEFAULT = 4096


class LLMProvider:
    """一个模型提供商配置（provider.json 条目）"""

    def __init__(self, data: dict):
        self.id = data.get("id", "unknown")
        self.name = data.get("name", self.id)
        self.format = data.get("format", "openai")          # openai / anthropic
        self.base_url = (data.get("base_url") or "").rstrip("/")
        self.api_key = data.get("api_key", "")
        self.model = data.get("model", "")
        self.context_window = data.get("context_window")     # None=无限
        self.supports_tools = data.get("supports_tools", True)
        self.supports_reasoning = data.get("supports_reasoning", True)
        self.extra = data.get("extra", {}) or {}

    def to_dict(self) -> dict:
        return {
            "id": self.id, "name": self.name, "format": self.format,
            "base_url": self.base_url, "api_key": self.api_key, "model": self.model,
            "context_window": self.context_window,
            "supports_tools": self.supports_tools,
            "supports_reasoning": self.supports_reasoning,
            "extra": self.extra,
        }


# ========== 全局状态 ==========
_providers: List[LLMProvider] = []
_active_provider_id: str = ""


def load_providers() -> List[LLMProvider]:
    """加载 provider.json（失败返回空列表——由 ai_server 用旧 deepseek/ollama 配置兜底）"""
    global _providers, _active_provider_id
    try:
        if os.path.exists(PROVIDER_FILE):
            with open(PROVIDER_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            plist = data.get("providers", []) if isinstance(data, dict) else []
            _providers = [LLMProvider(p) for p in plist
                          if isinstance(p, dict) and p.get("model") and p.get("base_url")]
            _active_provider_id = data.get("active", "") if isinstance(data, dict) else ""
            if _active_provider_id and not any(p.id == _active_provider_id for p in _providers):
                _active_provider_id = ""
            logger.info("llm_unified: loaded %d providers, active=%s", len(_providers),
                        _active_provider_id or "(none)")
            return _providers
    except Exception as e:
        logger.warning("llm_unified load_providers failed: %s", e)
    _providers = []
    _active_provider_id = ""
    return _providers


def save_providers(providers: List[LLMProvider], active_id: str = "") -> bool:
    """持久化 provider.json（App ai_config_set / model_switch 写入）"""
    global _providers, _active_provider_id
    _providers = providers
    _active_provider_id = active_id
    try:
        os.makedirs(os.path.dirname(PROVIDER_FILE), exist_ok=True)
        with open(PROVIDER_FILE, "w", encoding="utf-8") as f:
            json.dump({"providers": [p.to_dict() for p in providers], "active": active_id},
                      f, ensure_ascii=False, indent=2)
        logger.info("llm_unified: saved %d providers, active=%s", len(providers), active_id or "(none)")
        return True
    except Exception as e:
        logger.error("save_providers failed: %s", e)
        return False


def get_providers() -> List[LLMProvider]:
    return list(_providers)


def get_active_provider() -> Optional[LLMProvider]:
    for p in _providers:
        if p.id == _active_provider_id:
            return p
    return _providers[0] if _providers else None


def set_active_provider(pid: str) -> bool:
    """切换当前模型（model_switch 命令）"""
    global _active_provider_id
    if any(p.id == pid for p in _providers):
        _active_provider_id = pid
        save_providers(_providers, pid)
        return True
    return False


def get_context_window(provider: Optional[LLMProvider] = None) -> Optional[int]:
    """上下文窗口：显式配置 → 内置映射表 → /v1/models 查询 → None=无限"""
    p = provider or get_active_provider()
    if not p:
        return None
    if p.context_window:
        return int(p.context_window)
    model = (p.model or "").lower()
    if model in CONTEXT_WINDOW_MAP:
        return CONTEXT_WINDOW_MAP[model]
    if p.format == "openai" and p.base_url:
        try:
            import requests
            url = p.base_url.rstrip("/") + "/models"
            h = {"Authorization": "Bearer %s" % p.api_key} if p.api_key else {}
            r = requests.get(url, headers=h, timeout=5)
            if r.status_code == 200:
                for m in r.json().get("data", []):
                    if (m.get("id") or "").lower() == model:
                        cw = (m.get("context_window")
                              or (m.get("meta") or {}).get("context_length")
                              or (m.get("context_length")))
                        if cw:
                            p.context_window = int(cw)
                            return int(cw)
        except Exception:
            pass
    return None  # 无限


# ========== 消息规范化（HTTP 400 根因修复） ==========
def normalize_messages(messages: List[Dict]) -> List[Dict]:
    """发送前规范化（修复工具调用后续轮 400 的根因）：
    - role=tool 的 content 必须为非空字符串（部分 API 对空 content 直接 400）
    - role=assistant 带 tool_calls 时 content 必须为 ""（不能为 None）
    - content 非字符串统一转字符串
    """
    out = []
    for m in messages or []:
        m2 = dict(m)
        role = m2.get("role")
        if role == "tool":
            c = m2.get("content")
            if c is None or (isinstance(c, str) and not c.strip()):
                m2["content"] = "(no output)"
            elif not isinstance(c, str):
                m2["content"] = str(c)
        elif role == "assistant" and m2.get("tool_calls"):
            if "content" not in m2 or m2.get("content") is None:
                m2["content"] = ""
        elif role in ("user", "system") and m2.get("content") is None:
            m2["content"] = ""
        out.append(m2)
    return out


# ========== 调试落盘（先生要求：格式错误打印原始请求与服务端返回详细内容） ==========
def _mask_key(key: str) -> str:
    if not key:
        return ""
    if len(key) <= 8:
        return "***"
    return key[:4] + "***" + key[-4:]


def _dump_debug(provider: LLMProvider, req_body: dict, resp_status: int,
                resp_body: str, err_type: str) -> None:
    """格式错误落盘：完整请求体（Key 脱敏）+ 服务端返回详情"""
    try:
        os.makedirs(DEBUG_DIR, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        fname = "%s_%s.json" % (ts, provider.id)
        payload = {
            "ts": datetime.now().isoformat(),
            "provider": provider.id,
            "format": provider.format,
            "base_url": provider.base_url,
            "model": provider.model,
            "error_type": err_type,
            "http_status": resp_status,
            "request": req_body,
            "response": (resp_body or "")[:6000],
        }
        with open(os.path.join(DEBUG_DIR, fname), "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        logger.warning("API debug dumped: %s", os.path.join(DEBUG_DIR, fname))
    except Exception as e:
        logger.warning("dump_debug failed: %s", e)


# ========== 错误分类（中文详细说明 + 建议动作） ==========
def classify_http_error(status: int, provider: Optional[LLMProvider] = None) -> Tuple[str, str, str]:
    """HTTP 错误分类 → (error_type, message_zh, action_zh)"""
    pname = provider.name if provider else "模型"
    if status == 400:
        return ("InvalidRequest",
                "%s 拒绝了请求（HTTP 400）——请求格式可能不符合其 API 规范" % pname,
                "已打印原始请求体与服务端返回详情到日志 api_debug/，可据此排查具体字段错误")
    if status == 401:
        return ("AuthFailed",
                "%s API Key 无效（HTTP 401）" % pname,
                "请在 AI 配置 → 提供商中重新填写 API Key")
    if status == 402:
        return ("QuotaExceeded",
                "%s 余额或配额不足（HTTP 402）" % pname,
                "请检查账户余额/配额")
    if status == 403:
        return ("Forbidden",
                "%s 拒绝访问（HTTP 403）" % pname,
                "请检查 API Key 权限或地域限制")
    if status == 404:
        return ("NotFound",
                "%s 端点或模型不存在（HTTP 404）——当前 model=%s" % (pname, provider.model if provider else "?"),
                "请检查 base_url 与 model 名称是否正确")
    if status == 405:
        return ("MethodNotAllowed",
                "%s 不支持该请求方法（HTTP 405）" % pname,
                "请检查 base_url 是否指向正确的 API 端点（/chat/completions 或 /v1/messages）")
    if status == 413:
        return ("PayloadTooLarge",
                "请求体过大（HTTP 413）——消息超出服务端单请求限制" % pname,
                "上下文已压缩仍过大，请减少历史消息或切换更大窗口模型")
    if status == 429:
        return ("RateLimited",
                "%s 请求过于频繁（HTTP 429）" % pname,
                "已自动重试；仍失败请稍后再试或降低并发")
    if status >= 500:
        return ("ServerError",
                "%s 服务端错误（HTTP %d）" % (pname, status),
                "已自动重试；仍失败说明服务端异常，请稍后再试")
    return ("HttpError",
            "%s HTTP %d" % (pname, status),
            "请检查网络与配置，详情见 api_debug/")


def classify_exception(e: Exception, provider: Optional[LLMProvider] = None) -> Tuple[str, str, str]:
    s = str(e).lower()
    if "timeout" in s or "timed out" in s:
        return ("Timeout", "请求超时", "请检查网络连接或增大超时时间")
    if "connection" in s or "resolve" in s or "refused" in s or "unreachable" in s:
        return ("NetworkError", "网络连接失败（%s）" % e, "请检查主机网络与 base_url 可达性")
    return ("Unknown", "未知错误：%s" % e, "请查看日志 api_debug/ 获取详情")


# ========== OpenAI 格式 adapter（DeepSeek/Kimi/GLM/通义/Compatible/Ollama） ==========
def _openai_chat_url(provider: LLMProvider) -> str:
    base = provider.base_url.rstrip("/")
    if not base.endswith("/v1"):
        base = base + "/v1"
    return base + "/chat/completions"


def _openai_stream(provider: LLMProvider, messages: List[Dict], tools: Optional[List[Dict]],
                   timeout: int, temperature: Optional[float],
                   reasoning_effort: Optional[str]) -> Generator[Dict, None, None]:
    """OpenAI 兼容流式调用：yield thinking/content/tool_calls/usage 事件"""
    import requests
    url = _openai_chat_url(provider)
    headers = {"Content-Type": "application/json"}
    if provider.api_key:
        headers["Authorization"] = "Bearer %s" % provider.api_key
    payload = {
        "model": provider.model,
        "messages": normalize_messages(messages),
        "stream": True,
        "max_tokens": MAX_TOKENS_DEFAULT,
        "stream_options": {"include_usage": True},
    }
    if temperature is not None:
        payload["temperature"] = temperature
    if tools and provider.supports_tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"
    if reasoning_effort and provider.supports_reasoning:
        payload["reasoning_effort"] = reasoning_effort

    attempts = 0
    while True:
        attempts += 1
        try:
            with requests.post(url, headers=headers, json=payload, timeout=timeout, stream=True) as resp:
                if resp.status_code != 200:
                    body = ""
                    try:
                        body = resp.text[:3000]
                    except Exception:
                        pass
                    err_type, msg, action = classify_http_error(resp.status_code, provider)
                    # 【400 回退】带 reasoning_effort 失败 → 去掉重试一次
                    if (resp.status_code == 400 and payload.get("reasoning_effort")
                            and attempts == 1):
                        logger.warning("400 with reasoning_effort, retrying without it: %s", provider.model)
                        _dump_debug(provider, payload, resp.status_code, body, "InvalidRequest_retry")
                        payload.pop("reasoning_effort", None)
                        continue
                    if resp.status_code in (429,) or resp.status_code >= 500:
                        if attempts == 1:
                            time.sleep(1.5)
                            continue
                    _dump_debug(provider, payload, resp.status_code, body, err_type)
                    yield {"type": "error", "text": "%s。%s" % (msg, action),
                           "error_type": err_type, "action": action}
                    return
                usage = None
                tool_calls_acc: Dict[int, dict] = {}
                for line in resp.iter_lines():
                    if not line or not line.startswith(b"data: "):
                        continue
                    chunk = line[6:]
                    if chunk == b"[DONE]":
                        break
                    try:
                        data = json.loads(chunk)
                    except Exception:
                        continue
                    choices = data.get("choices") or []
                    if not choices and data.get("usage"):
                        usage = data["usage"]
                        continue
                    delta = choices[0].get("delta", {}) if choices else {}
                    reasoning = delta.get("reasoning_content", "")
                    if reasoning:
                        yield {"type": "thinking", "text": reasoning}
                    content = delta.get("content")
                    if content:
                        yield {"type": "content", "text": content}
                    tc_delta = delta.get("tool_calls")
                    if tc_delta:
                        for tc in tc_delta:
                            idx = tc.get("index", 0)
                            acc = tool_calls_acc.setdefault(idx, {"name": "", "arguments": ""})
                            fn = tc.get("function", {})
                            if fn.get("name"):
                                acc["name"] += fn["name"]
                            if fn.get("arguments"):
                                acc["arguments"] += fn["arguments"]
                if tool_calls_acc:
                    tcs = [{"type": "function", "id": "call_%d" % i,
                            "function": {"name": tool_calls_acc[i]["name"],
                                         "arguments": tool_calls_acc[i]["arguments"] or "{}"}}
                           for i in sorted(tool_calls_acc)]
                    yield {"type": "tool_calls", "tool_calls": tcs}
                if usage:
                    yield {"type": "usage", "usage": {
                        "prompt_tokens": usage.get("prompt_tokens", 0),
                        "completion_tokens": usage.get("completion_tokens", 0),
                        "total_tokens": usage.get("total_tokens", 0),
                        "prompt_cache_hit_tokens": usage.get("prompt_cache_hit_tokens", 0),
                    }}
                return
        except Exception as e:
            err_type, msg, action = classify_exception(e, provider)
            if attempts == 1 and err_type in ("Timeout", "NetworkError"):
                logger.warning("llm %s: %s, retrying once", provider.id, msg)
                time.sleep(1.0)
                continue
            _dump_debug(provider, payload, 0, "exception: %s" % e, err_type)
            yield {"type": "error", "text": "%s。%s" % (msg, action),
                   "error_type": err_type, "action": action}
            return


def _openai_nonstream(provider: LLMProvider, messages: List[Dict], tools: Optional[List[Dict]],
                      timeout: int, temperature: Optional[float],
                      reasoning_effort: Optional[str]) -> Dict:
    """OpenAI 兼容非流式调用 → {"content": str, "usage": {...}, "tool_calls": [...]}"""
    import requests
    url = _openai_chat_url(provider)
    headers = {"Content-Type": "application/json"}
    if provider.api_key:
        headers["Authorization"] = "Bearer %s" % provider.api_key
    payload = {
        "model": provider.model,
        "messages": normalize_messages(messages),
        "stream": False,
        "max_tokens": MAX_TOKENS_DEFAULT,
    }
    if temperature is not None:
        payload["temperature"] = temperature
    if tools and provider.supports_tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"
    if reasoning_effort and provider.supports_reasoning:
        payload["reasoning_effort"] = reasoning_effort

    attempts = 0
    while True:
        attempts += 1
        try:
            resp = requests.post(url, headers=headers, json=payload, timeout=timeout)
            if resp.status_code != 200:
                err_type, msg, action = classify_http_error(resp.status_code, provider)
                if (resp.status_code == 400 and payload.get("reasoning_effort") and attempts == 1):
                    logger.warning("nonstream 400 with reasoning_effort, retry without: %s", provider.model)
                    payload.pop("reasoning_effort", None)
                    continue
                if (resp.status_code in (429,) or resp.status_code >= 500) and attempts == 1:
                    time.sleep(1.5)
                    continue
                _dump_debug(provider, payload, resp.status_code, resp.text, err_type)
                return {"content": "%s。%s" % (msg, action), "error_type": err_type,
                        "action": action, "error": True}
            data = resp.json()
            choice = (data.get("choices") or [{}])[0]
            message = choice.get("message", {})
            usage = data.get("usage", {})
            content = message.get("content") or ""
            reasoning = message.get("reasoning_content") or ""
            tcs = message.get("tool_calls")
            return {"content": content, "reasoning": reasoning, "usage": usage,
                    "tool_calls": tcs}
        except Exception as e:
            err_type, msg, action = classify_exception(e, provider)
            if attempts == 1 and err_type in ("Timeout", "NetworkError"):
                time.sleep(1.0)
                continue
            _dump_debug(provider, payload, 0, "exception: %s" % e, err_type)
            return {"content": "%s。%s" % (msg, action), "error_type": err_type,
                    "action": action, "error": True}


# ========== Anthropic 格式 adapter（原生 Messages API） ==========
def _convert_to_anthropic(messages: List[Dict]) -> Tuple[str, List[Dict]]:
    """OpenAI 消息 → Anthropic Messages（system 抽出；tool_calls → tool_use；tool → tool_result）"""
    system_parts = []
    conv = []
    for m in messages or []:
        role = m.get("role")
        content = m.get("content") or ""
        if role == "system":
            system_parts.append(str(content))
            continue
        if role == "user":
            conv.append({"role": "user", "content": str(content)})
        elif role == "assistant":
            blocks = []
            if content:
                blocks.append({"type": "text", "text": str(content)})
            for tc in m.get("tool_calls") or []:
                fn = tc.get("function", {})
                blocks.append({
                    "type": "tool_use",
                    "id": tc.get("id") or ("call_%d" % len(blocks)),
                    "name": fn.get("name", ""),
                    "input": _safe_json(fn.get("arguments", "{}")),
                })
            conv.append({"role": "assistant", "content": blocks})
        elif role == "tool":
            conv.append({"role": "user", "content": [
                {"type": "tool_result", "tool_use_id": m.get("tool_call_id", ""),
                 "content": str(content) or "(no output)"}
            ]})
    return "\n\n".join(system_parts), conv


def _safe_json(s: str) -> dict:
    try:
        v = json.loads(s)
        return v if isinstance(v, dict) else {"value": v}
    except Exception:
        return {}


def _anthropic_stream(provider: LLMProvider, messages: List[Dict], tools: Optional[List[Dict]],
                      timeout: int, temperature: Optional[float]) -> Generator[Dict, None, None]:
    """Anthropic 原生流式：SSE 解析（thinking_delta/text_delta/input_json_delta/tool_use）"""
    import requests
    base = provider.base_url.rstrip("/")
    if not base.endswith("/v1"):
        base = base + "/v1"
    url = base + "/messages"
    headers = {
        "Content-Type": "application/json",
        "x-api-key": provider.api_key or "",
        "anthropic-version": "2023-06-01",
    }
    system_text, conv = _convert_to_anthropic(messages)
    payload = {"model": provider.model, "messages": conv, "max_tokens": MAX_TOKENS_DEFAULT,
               "stream": True}
    if system_text:
        payload["system"] = system_text
    if temperature is not None:
        payload["temperature"] = temperature
    if tools and provider.supports_tools:
        payload["tools"] = [_anth_tool_schema(t) for t in tools]

    attempts = 0
    while True:
        attempts += 1
        try:
            with requests.post(url, headers=headers, json=payload, timeout=timeout, stream=True) as resp:
                if resp.status_code != 200:
                    body = ""
                    try:
                        body = resp.text[:3000]
                    except Exception:
                        pass
                    err_type, msg, action = classify_http_error(resp.status_code, provider)
                    if (resp.status_code in (429,) or resp.status_code >= 500) and attempts == 1:
                        time.sleep(1.5)
                        continue
                    _dump_debug(provider, payload, resp.status_code, body, err_type)
                    yield {"type": "error", "text": "%s。%s" % (msg, action),
                           "error_type": err_type, "action": action}
                    return
                usage_in = 0
                usage_out = 0
                tool_blocks: Dict[int, dict] = {}
                current_block_idx = -1
                for raw in resp.iter_lines():
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="ignore")
                    if not line.startswith("data:"):
                        continue
                    d = line[5:].strip()
                    if not d:
                        continue
                    try:
                        evt = json.loads(d)
                    except Exception:
                        continue
                    etype = evt.get("type")
                    if etype == "message_start":
                        u = evt.get("message", {}).get("usage", {})
                        usage_in = u.get("input_tokens", 0)
                    elif etype == "content_block_start":
                        cb = evt.get("content_block", {})
                        current_block_idx = evt.get("index", -1)
                        if cb.get("type") == "tool_use":
                            tool_blocks[current_block_idx] = {
                                "id": cb.get("id", ""), "name": cb.get("name", ""),
                                "arguments": "",
                            }
                    elif etype == "content_block_delta":
                        delta = evt.get("delta", {})
                        dtype = delta.get("type")
                        if dtype == "thinking_delta" and delta.get("thinking"):
                            yield {"type": "thinking", "text": delta["thinking"]}
                        elif dtype == "text_delta" and delta.get("text"):
                            yield {"type": "content", "text": delta["text"]}
                        elif dtype == "input_json_delta" and delta.get("partial_json"):
                            tb = tool_blocks.get(current_block_idx)
                            if tb:
                                tb["arguments"] += delta["partial_json"]
                    elif etype == "message_delta":
                        u = evt.get("usage", {})
                        usage_out = u.get("output_tokens", 0)
                if tool_blocks:
                    tcs = [{"type": "function", "id": tb.get("id") or "call_%d" % i,
                            "function": {"name": tb.get("name", ""),
                                         "arguments": tb.get("arguments") or "{}"}}
                           for i, tb in sorted(tool_blocks.items())]
                    yield {"type": "tool_calls", "tool_calls": tcs}
                yield {"type": "usage", "usage": {
                    "prompt_tokens": usage_in, "completion_tokens": usage_out,
                    "total_tokens": usage_in + usage_out,
                    "prompt_cache_hit_tokens": 0,
                }}
                return
        except Exception as e:
            err_type, msg, action = classify_exception(e, provider)
            if attempts == 1 and err_type in ("Timeout", "NetworkError"):
                time.sleep(1.0)
                continue
            _dump_debug(provider, payload, 0, "exception: %s" % e, err_type)
            yield {"type": "error", "text": "%s。%s" % (msg, action),
                   "error_type": err_type, "action": action}
            return


def _anth_tool_schema(t: dict) -> dict:
    """OpenAI tool schema → Anthropic tool schema"""
    fn = t.get("function", {})
    return {
        "name": fn.get("name", ""),
        "description": fn.get("description", ""),
        "input_schema": fn.get("parameters") or {"type": "object", "properties": {}},
    }


def _anthropic_nonstream(provider: LLMProvider, messages: List[Dict], tools: Optional[List[Dict]],
                         timeout: int, temperature: Optional[float]) -> Dict:
    """Anthropic 非流式 → {"content", "usage", "tool_calls"}"""
    import requests
    base = provider.base_url.rstrip("/")
    if not base.endswith("/v1"):
        base = base + "/v1"
    url = base + "/messages"
    headers = {
        "Content-Type": "application/json",
        "x-api-key": provider.api_key or "",
        "anthropic-version": "2023-06-01",
    }
    system_text, conv = _convert_to_anthropic(messages)
    payload = {"model": provider.model, "messages": conv, "max_tokens": MAX_TOKENS_DEFAULT}
    if system_text:
        payload["system"] = system_text
    if temperature is not None:
        payload["temperature"] = temperature
    if tools and provider.supports_tools:
        payload["tools"] = [_anth_tool_schema(t) for t in tools]

    attempts = 0
    while True:
        attempts += 1
        try:
            resp = requests.post(url, headers=headers, json=payload, timeout=timeout)
            if resp.status_code != 200:
                err_type, msg, action = classify_http_error(resp.status_code, provider)
                if (resp.status_code in (429,) or resp.status_code >= 500) and attempts == 1:
                    time.sleep(1.5)
                    continue
                _dump_debug(provider, payload, resp.status_code, resp.text, err_type)
                return {"content": "%s。%s" % (msg, action), "error_type": err_type,
                        "action": action, "error": True}
            data = resp.json()
            usage = data.get("usage", {})
            tcs = []
            text_parts = []
            for block in data.get("content", []):
                if block.get("type") == "text":
                    text_parts.append(block.get("text", ""))
                elif block.get("type") == "tool_use":
                    tcs.append({"type": "function",
                                "id": block.get("id", ""),
                                "function": {"name": block.get("name", ""),
                                             "arguments": json.dumps(block.get("input", {}), ensure_ascii=False)}})
            return {"content": "".join(text_parts), "usage": {
                "prompt_tokens": usage.get("input_tokens", 0),
                "completion_tokens": usage.get("output_tokens", 0),
                "total_tokens": usage.get("input_tokens", 0) + usage.get("output_tokens", 0),
                "prompt_cache_hit_tokens": usage.get("cache_read_input_tokens", 0),
            }, "tool_calls": tcs}
        except Exception as e:
            err_type, msg, action = classify_exception(e, provider)
            if attempts == 1 and err_type in ("Timeout", "NetworkError"):
                time.sleep(1.0)
                continue
            _dump_debug(provider, payload, 0, "exception: %s" % e, err_type)
            return {"content": "%s。%s" % (msg, action), "error_type": err_type,
                    "action": action, "error": True}


# ========== 统一入口 ==========
def call_llm_stream(messages: List[Dict], tools: Optional[List[Dict]] = None,
                    provider: Optional[LLMProvider] = None, timeout: int = 120,
                    temperature: Optional[float] = None,
                    reasoning_effort: Optional[str] = None) -> Generator[Dict, None, None]:
    """统一流式调用：yield {"type": "thinking"|"content"|"tool_calls"|"usage"|"error", ...}"""
    p = provider or get_active_provider()
    if not p:
        yield {"type": "error", "text": "未配置任何模型提供商——请在 AI 配置中添加", "error_type": "NoProvider",
               "action": "AI 配置 → 添加提供商"}
        return
    if p.format == "anthropic":
        yield from _anthropic_stream(p, messages, tools, timeout, temperature)
    else:
        yield from _openai_stream(p, messages, tools, timeout, temperature, reasoning_effort)


def call_llm_nonstream(messages: List[Dict], tools: Optional[List[Dict]] = None,
                       provider: Optional[LLMProvider] = None, timeout: int = 120,
                       temperature: Optional[float] = None,
                       reasoning_effort: Optional[str] = None) -> Dict:
    """统一非流式调用 → {"content":..., "usage":..., "tool_calls":..., "error": bool}"""
    p = provider or get_active_provider()
    if not p:
        return {"content": "未配置任何模型提供商——请在 AI 配置中添加", "error": True}
    if p.format == "anthropic":
        return _anthropic_nonstream(p, messages, tools, timeout, temperature)
    return _openai_nonstream(p, messages, tools, timeout, temperature, reasoning_effort)
