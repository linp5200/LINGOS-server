#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS HA (Help AI) 事件归档系统
版本: LN-B-5.0.0.0-rc0.4
功能：AI 自主 + 系统自动 构建事件档案（/LINGOS/HA/），帮助未来 AI 决策
  - 技术/系统事件（错误根因、修复方案、成功模式）→ 入 HA
  - 用户相关内容 → 记忆系统（memory_*）
核心协议：C1（容错/日志）、C-C（容错编程/跛脚编程）、UD-DR#S1（与记忆分工）
"""

import os
import json
import time
import uuid
import logging

logger = logging.getLogger("HAArchive")

HA_ROOT = "/LINGOS/HA"
DEFAULT_TTL = 90 * 24 * 3600   # 默认保留 90 天


def _ensure_dir(path):
    """确保目录存在（容错）"""
    try:
        os.makedirs(path, exist_ok=True)
        return True
    except Exception as e:
        logger.warning("HA mkdir failed: %s", e)
        return False


def ha_write_event(event_type="info", title="", summary="", analysis="",
                   source="ai:nook", severity="info", payload=None,
                   ttl=DEFAULT_TTL):
    """创建事件文件：/LINGOS/HA/YYYY-MM-DD/evt_<ts>_<uuid8>.json

    :param event_type: success | failure | info | warning
    :param title: 事件标题（简短）
    :param summary: AI 构建的事件摘要
    :param analysis: AI 构建的根因分析/建议（失败事件）
    :param source: 来源（ai:nook / agent:ai_code / system:lingosd ...）
    :param severity: info | warn | critical
    :param payload: 附加数据（可选）
    :param ttl: 保留时长（秒），默认 90 天
    :return: 事件 ID，失败返回 None
    """
    try:
        ts = int(time.time())
        date_str = time.strftime("%Y-%m-%d", time.localtime(ts))
        day_dir = os.path.join(HA_ROOT, date_str)
        if not _ensure_dir(day_dir):
            return None

        event_id = "evt_%d_%s" % (ts, uuid.uuid4().hex[:8])
        evt = {
            "id": event_id,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts)),
            "type": event_type if event_type in ("success", "failure", "info", "warning") else "info",
            "severity": severity if severity in ("info", "warn", "critical") else "info",
            "source": source or "system",
            "title": (title or "")[:200],
            "summary": (summary or "")[:2000],
            "analysis": (analysis or "")[:2000],
            "payload": payload or {},
            "ttl": ttl
        }

        path = os.path.join(day_dir, event_id + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(evt, f, ensure_ascii=False, indent=2)
        logger.info("HA event written: %s", path)
        return event_id
    except Exception as e:
        logger.error("HA event write failed: %s", e)
        return None


def ha_search(query="", limit=20, event_type=None, source=None):
    """检索 HA 事件（供 AI 参考历史成功/失败模式）

    :param query: 关键词过滤（匹配 title/summary/analysis）
    :param limit: 返回上限
    :param event_type: success/failure/info/warning 过滤
    :param source: 来源过滤（如 system:lingosd）
    :return: 事件列表（新→旧）
    """
    if not os.path.isdir(HA_ROOT):
        return []
    results = []
    try:
        # 收集所有事件文件（按时间戳倒序）
        all_events = []
        for root, _dirs, files in os.walk(HA_ROOT):
            for fn in files:
                if not fn.endswith(".json"):
                    continue
                try:
                    with open(os.path.join(root, fn), "r", encoding="utf-8") as f:
                        evt = json.load(f)
                    all_events.append(evt)
                except Exception:
                    continue
        all_events.sort(key=lambda e: e.get("timestamp", ""), reverse=True)

        for evt in all_events:
            if event_type and evt.get("type") != event_type:
                continue
            if source and evt.get("source") != source:
                continue
            if query:
                hay = " ".join([
                    str(evt.get("title", "")),
                    str(evt.get("summary", "")),
                    str(evt.get("analysis", ""))
                ])
                if query.lower() not in hay.lower():
                    continue
            results.append(evt)
            if len(results) >= limit:
                break
    except Exception as e:
        logger.warning("HA search failed: %s", e)
    return results


def ha_stats():
    """归档统计（供状态显示）"""
    total = 0
    by_type = {}
    try:
        for _root, _dirs, files in os.walk(HA_ROOT):
            for fn in files:
                if fn.endswith(".json"):
                    total += 1
                    try:
                        with open(os.path.join(_root, fn), "r", encoding="utf-8") as f:
                            evt = json.load(f)
                        t = evt.get("type", "info")
                        by_type[t] = by_type.get(t, 0) + 1
                    except Exception:
                        pass
    except Exception:
        pass
    return {"total_events": total, "by_type": by_type, "root": HA_ROOT}
