#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 自动记忆管线（P3 · 对标 mem0——对话→事实抽取→去重→分级写入）

先生设定（方案2 §8 设计一）：
  触发：对话结束 / 每 N 轮
  流程：对话片段 → 抽取（"事实/偏好/决策"）→ 去重 → 写入（L2 工作级）
  开关：选项 ai.auto_memory（默认关——隐私默认）；本模块读选项，不可读时读配置
  可选 LLM 抽取：配置 llm_extract=true 时启用（默认启发式——确定性、零依赖）

配置：/LINGOS/system/config/memory_pipeline.json
  {"enabled":null, "llm_extract":false, "min_chars":12, "max_facts":3, "turns_interval":1}
"""

import json
import logging
import os
import re
import threading
import time

logger = logging.getLogger("MemoryPipeline")

CONFIG_PATH = "/LINGOS/system/config/memory_pipeline.json"
STATE_PATH = "/LINGOS/state/memory_pipeline_state.json"

_lock = threading.RLock()
_turn_counter = {}

_DEFAULTS = {
    "enabled": None,        # None=跟随选项 ai.auto_memory；true/false=显式覆盖
    "llm_extract": False,   # 可选 LLM 抽取（默认关——启发式已覆盖高频场景）
    "min_chars": 12,
    "max_facts": 3,
    "turns_interval": 1,    # 每 N 轮抽一次（1=每轮）
}


def _load_cfg() -> dict:
    cfg = dict(_DEFAULTS)
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                d = json.load(f) or {}
            if isinstance(d, dict):
                cfg.update(d)
    except Exception as e:
        logger.debug("memory_pipeline config: %s", e)
    return cfg


def _option_enabled() -> bool:
    """读选项 ai.auto_memory（经 C 端 options_get 系统调用——失败=关）"""
    try:
        from syscall_client import call_syscall
        ok, out = call_syscall("options_get", {"key": "ai.auto_memory"}, timeout=3)
        if ok and out:
            try:
                d = json.loads(out)
                v = d.get("data", d.get("value"))
                if isinstance(v, dict):
                    v = v.get("value")
                return bool(v)
            except Exception:
                pass
    except Exception:
        pass
    return False


def _enabled(cfg: dict) -> bool:
    if cfg.get("enabled") is True:
        return True
    if cfg.get("enabled") is False:
        return False
    return _option_enabled()


# -------------------------------------------------------------
# 事实抽取（启发式——中英双语；可选 LLM）
# -------------------------------------------------------------
_PATTERNS = [
    re.compile(r"(记住|记一下|请记得|帮我记住)[：:，,]?\s*(.{4,120})"),
    re.compile(r"(remember(?: that)?|note that)[：:,]?\s*(.{4,160})", re.I),
    re.compile(r"(我(?:更)?(?:喜欢|偏好|习惯|通常|总是|不喜欢|讨厌))([^。！？\n]{2,100})"),
    re.compile(r"(我的(?:名字|偏好|习惯|账号?|地址|日程)[^。！？\n]{2,100})"),
    re.compile(r"(以后(?:都|请|不要|别)[^。！？\n]{2,100})"),
    re.compile(r"(叫我[^。！？\n]{1,30})"),
    re.compile(r"(重要[：:]\s*[^。！？\n]{4,150})"),
]


def _extract_heuristic(user_text: str, assistant_text: str) -> list:
    facts = []
    text = user_text or ""
    for pat in _PATTERNS:
        for m in pat.finditer(text):
            g = m.groups()
            frag = (g[-1] if g and g[-1] else m.group(0)) or ""
            # 保留触发词上下文（便于检索）
            head = m.group(0)[: len(m.group(0)) - len(frag)]
            fact = (head + frag).strip(" ：:，,")
            if len(fact) >= 6:
                facts.append(fact)
    # 去重（本批内）
    seen, out = set(), []
    for f in facts:
        k = f[:24]
        if k not in seen:
            seen.add(k)
            out.append(f)
    return out[:3]


def _extract_llm(user_text: str, assistant_text: str, max_facts: int) -> list:
    """可选 LLM 抽取（llm_extract=true 时）——短超时，失败回退启发式"""
    try:
        from llm_unified import call_llm_nonstream
        prompt = (
            "从下面对话中抽取用户的事实/偏好/决定（最多 %d 条，每条≤80字；"
            "只输出 JSON 数组，无其他文字；没有则输出 []）。\n\n用户: %s\n助手: %s"
            % (max_facts, (user_text or "")[:600], (assistant_text or "")[:400])
        )
        r = call_llm_nonstream(
            [{"role": "system", "content": "You extract structured facts. Output JSON array only."},
             {"role": "user", "content": prompt}],
            timeout=20, temperature=0.2)
        content = (r.get("content") or "").strip()
        if content.startswith("```"):
            content = content.strip("`")
            if content.startswith("json"):
                content = content[4:]
        arr = json.loads(content)
        if isinstance(arr, list):
            return [str(x)[:120] for x in arr if str(x).strip()][:max_facts]
    except Exception as e:
        logger.debug("llm extract failed: %s", e)
    return []


# -------------------------------------------------------------
# 去重（与已有记忆比对——近似包含判定）
# -------------------------------------------------------------
def _is_duplicate(fact: str) -> bool:
    try:
        from ai_server import cmd_memory_search
        key = fact[:10]
        res = cmd_memory_search(key)
        items = []
        if isinstance(res, dict):
            d = res.get("data")
            if isinstance(d, list):
                items = d
            elif isinstance(d, dict):
                items = d.get("items", [])
        for it in items[:8]:
            content = str(it.get("content", "")) if isinstance(it, dict) else str(it)
            if not content:
                continue
            # 近似：新事实与新记忆互含 ≥8 字公共前缀或高重叠
            a, b = fact.replace(" ", ""), content.replace(" ", "")
            if len(a) >= 8 and (a in b or b in a):
                return True
            common = sum(1 for i in range(min(len(a), len(b))) if a[i] == b[i])
            if common >= min(len(a), len(b)) * 0.8 and common >= 10:
                return True
    except Exception as e:
        logger.debug("dedupe check failed: %s", e)
    return False


def _write_fact(fact: str) -> bool:
    try:
        from ai_server import cmd_memory_write
        r = cmd_memory_write(fact, "medium")
        ok = isinstance(r, dict) and r.get("status") == "ok"
        if ok:
            logger.info("auto-memory written: %s", fact[:60])
        return ok
    except Exception as e:
        logger.warning("auto-memory write failed: %s", e)
        return False


# -------------------------------------------------------------
# 入口（对话完成时调用）
# -------------------------------------------------------------
def maybe_extract(session_id: str, user_text: str, assistant_text: str = "") -> dict:
    """对话结束钩子——按开关/频率决定是否抽取并写入"""
    try:
        cfg = _load_cfg()
        if not _enabled(cfg):
            return {"status": "ok", "skipped": "disabled"}
        if not user_text or len(user_text) < int(cfg.get("min_chars", 12)):
            return {"status": "ok", "skipped": "too_short"}

        with _lock:
            n = _turn_counter.get(session_id, 0) + 1
            _turn_counter[session_id] = n
            interval = max(1, int(cfg.get("turns_interval", 1)))
            if n % interval != 0:
                return {"status": "ok", "skipped": "interval"}

        max_facts = max(1, int(cfg.get("max_facts", 3)))
        facts = []
        if cfg.get("llm_extract"):
            facts = _extract_llm(user_text, assistant_text, max_facts)
        if not facts:
            facts = _extract_heuristic(user_text, assistant_text)[:max_facts]
        if not facts:
            return {"status": "ok", "skipped": "no_fact"}

        written = 0
        for f in facts:
            if _is_duplicate(f):
                logger.debug("duplicate fact skipped: %s", f[:40])
                continue
            if _write_fact(f):
                written += 1

        # 状态持久化（调试用）
        try:
            os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
            with open(STATE_PATH, "w", encoding="utf-8") as sf:
                json.dump({"last_run": time.time(), "session": session_id,
                           "facts": facts, "written": written}, sf, ensure_ascii=False)
        except Exception:
            pass

        return {"status": "ok", "extracted": len(facts), "written": written}
    except Exception as e:
        logger.warning("memory pipeline error: %s", e)
        return {"status": "error", "msg": str(e)}
