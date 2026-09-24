#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 预警订阅 + AI 简报（P3 · 方案2 §3 设计三/四）

功能：
  ① 预警级别订阅：按级别阈值过滤通知推送（L2+ 默认；可配）
  ② AI 简报：聚合近段预警 → 生成摘要 → 推送通知中心（冷却 + 最小条数）
     简报生成：LLM 可用时用 LLM（短超时）；失败回退模板聚合（诚实降级）

配置：/LINGOS/system/config/alert_subscription.json
  {"min_level": 2, "notify": true,
   "brief": {"enabled": true, "interval_h": 6, "min_alerts": 2, "llm": true}}

数据：/LINGOS/data/alerts_recent.jsonl（滚动，最多 300 条）
状态：/LINGOS/state/alert_brief_state.json
"""

import json
import logging
import os
import time

logger = logging.getLogger("AlertSub")

CONFIG_PATH = "/LINGOS/system/config/alert_subscription.json"
RECENT_PATH = "/LINGOS/data/alerts_recent.jsonl"
STATE_PATH = "/LINGOS/state/alert_brief_state.json"

_DEFAULTS = {
    "min_level": 2,
    "notify": True,
    "brief": {"enabled": True, "interval_h": 6, "min_alerts": 2, "llm": True},
}

_LEVEL_NAME = {0: "info", 1: "info", 2: "warn", 3: "error", 4: "critical", 5: "critical"}

_TYPE_ZH = {
    "typhoon": "台风", "earthquake": "地震", "rain": "暴雨", "high_temp": "高温",
    "storm": "风暴", "fire": "火灾", "health": "系统健康", "security": "安全威胁",
    "intrusion": "入侵",
}


def _load_cfg() -> dict:
    cfg = json.loads(json.dumps(_DEFAULTS))   # deep copy
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                d = json.load(f) or {}
            if isinstance(d, dict):
                for k, v in d.items():
                    if k == "brief" and isinstance(v, dict):
                        cfg["brief"].update(v)
                    else:
                        cfg[k] = v
    except Exception as e:
        logger.debug("alert_sub config: %s", e)
    return cfg


def _save_state(st: dict) -> None:
    try:
        os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump(st, f, ensure_ascii=False)
    except Exception:
        pass


def _load_state() -> dict:
    try:
        if os.path.exists(STATE_PATH):
            with open(STATE_PATH, encoding="utf-8") as f:
                return json.load(f) or {}
    except Exception:
        pass
    return {}


def _append_recent(alert: dict) -> None:
    try:
        os.makedirs(os.path.dirname(RECENT_PATH), exist_ok=True)
        with open(RECENT_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps({
                "ts": int(time.time()),
                "type": str(alert.get("type", "")),
                "level": int(alert.get("level", 0) or 0),
                "source": str(alert.get("source", "")),
                "desc": str(alert.get("description", ""))[:200],
            }, ensure_ascii=False) + "\n")
        # 滚动裁剪（>300 行时保留后 200）
        try:
            with open(RECENT_PATH, encoding="utf-8") as f:
                lines = f.readlines()
            if len(lines) > 300:
                with open(RECENT_PATH, "w", encoding="utf-8") as f:
                    f.writelines(lines[-200:])
        except Exception:
            pass
    except Exception as e:
        logger.debug("recent append: %s", e)


def _recent_since(ts: int) -> list:
    out = []
    try:
        if not os.path.exists(RECENT_PATH):
            return out
        with open(RECENT_PATH, encoding="utf-8") as f:
            for line in f:
                try:
                    d = json.loads(line)
                    if int(d.get("ts", 0)) >= ts:
                        out.append(d)
                except Exception:
                    continue
    except Exception:
        pass
    return out


# -------------------------------------------------------------
# AI 简报（LLM 优先，模板降级）
# -------------------------------------------------------------
def _brief_template(items: list) -> str:
    from collections import Counter
    by_type = Counter(_TYPE_ZH.get(x.get("type", ""), x.get("type", "") or "其他") for x in items)
    max_lv = max((int(x.get("level", 0)) for x in items), default=0)
    parts = ["共 %d 条预警（最高 L%d）" % (len(items), max_lv)]
    parts.append("类别：" + "、".join("%s×%d" % (k, v) for k, v in by_type.most_common(5)))
    latest = items[-1]
    parts.append("最新：%s" % (latest.get("desc", "")[:80]))
    return "；".join(parts)


def _brief_llm(items: list) -> str:
    try:
        from llm_unified import call_llm_nonstream
        lines = "\n".join(
            "- [L%d][%s] %s (%s)" % (x.get("level", 0), _TYPE_ZH.get(x.get("type", ""), x.get("type", "")),
                                     x.get("desc", ""), x.get("source", ""))
            for x in items[-20:]
        )
        r = call_llm_nonstream(
            [{"role": "system", "content": "你是预警简报助手。用中文输出 2-4 句简洁简报：总体态势、重点类别、建议关注。只输出简报正文。"},
             {"role": "user", "content": "近段预警如下：\n" + lines}],
            timeout=25, temperature=0.3)
        content = (r.get("content") or "").strip()
        if content and not r.get("error"):
            return content[:600]
    except Exception as e:
        logger.debug("brief llm failed: %s", e)
    return ""


def _maybe_brief(cfg: dict, state: dict) -> None:
    b = cfg.get("brief", {})
    if not b.get("enabled", True):
        return
    interval = max(1, int(b.get("interval_h", 6))) * 3600
    last = int(state.get("last_brief_ts", 0))
    now = int(time.time())
    if now - last < interval:
        return
    items = _recent_since(last)
    if len(items) < int(b.get("min_alerts", 2)):
        return

    text = ""
    if b.get("llm", True):
        text = _brief_llm(items)
    if not text:
        text = _brief_template(items)

    try:
        from ux_ext import cmd_notify_push
        cmd_notify_push(title="📋 预警简报", body=text, level="info", source="alert_brief")
    except Exception as e:
        logger.debug("brief push failed: %s", e)

    state["last_brief_ts"] = now
    _save_state(state)
    logger.info("alert brief pushed (%d alerts)", len(items))


# -------------------------------------------------------------
# 入口（alertd 上报时调用）
# -------------------------------------------------------------
def on_alert_for_notify(alert: dict) -> dict:
    """预警到达 → 订阅过滤 → 通知推送 + 简报判定"""
    try:
        cfg = _load_cfg()
        alert = alert or {}
        level = int(alert.get("level", 0) or 0)
        _append_recent(alert)

        pushed = False
        if cfg.get("notify", True) and level >= int(cfg.get("min_level", 2)):
            try:
                from ux_ext import cmd_notify_push
                tname = _TYPE_ZH.get(str(alert.get("type", "")), str(alert.get("type", "")))
                title = "⚠️ %s预警（L%d）" % (tname, level)
                body = str(alert.get("description", ""))[:300]
                if alert.get("source"):
                    body += "\n来源: %s" % alert.get("source")
                cmd_notify_push(title=title, body=body,
                                level=_LEVEL_NAME.get(level, "warn"), source="alert")
                pushed = True
            except Exception as e:
                logger.debug("alert notify push failed: %s", e)

        _maybe_brief(cfg, _load_state())
        return {"status": "ok", "pushed": pushed, "level": level}
    except Exception as e:
        logger.warning("alert subscription error: %s", e)
        return {"status": "error", "msg": str(e)}
