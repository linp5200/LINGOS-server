#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 预警记录读取与统计（Python 侧单一事实源）

数据真源（C 侧写入）：
  /LINGOS/data/alerts/<YYYY-MM-DD>/<ts>_<type>.json
    由 src/alert/alert_history.c 写入，字段：
      title / content / time / ts / level / level_num / type / type_name /
      source / location / description / latitude / longitude / distance_km / timestamp
  /LINGOS/data/alerts/urgent/*.json —— L0 紧急通道

级别标签与 C 侧 src/alert/alert_utils.c:alert_level_label 完全一致：
  info(L0 级含义=提示) 、L0（最严重，紧急通道）、L1、L2、L3、L4、L5

历史事故（task-10 ①）：ai_server.cmd_alert_summary 的级别桶只有 L0~L3 + info，
C 端已写 L4/L5 → 被统计进 info，App 预警页把 L4/L5 显示成提示级（生命线显示错误）。
本模块把级别归一化与统计收敛到一处，供命令面与技能共用。
"""

import os
import json
import time
import logging
from typing import Dict, List, Optional

logger = logging.getLogger("AlertStore")

ALERT_BASE = "/LINGOS/data/alerts"
URGENT_DIR = os.path.join(ALERT_BASE, "urgent")

# 完整级别集（与 C alert_level_label 一致）；顺序 = 严重度降序（L0 最严重）
LEVEL_ORDER = ("L0", "L1", "L2", "L3", "L4", "L5", "info")

# 技能 type 参数别名 → C 侧 alert_type_t（alert_utils.c:alert_type_label）
TYPE_ALIASES = {
    "typhoon": 1, "台风": 1,
    "earthquake": 2, "地震": 2,
    "rain": 3, "暴雨": 3,
    "high temp": 4, "high_temp": 4, "高温": 4,
    "storm": 5, "风暴": 5,
    "fire": 6, "火灾": 6,
    "system health": 7, "system_health": 7, "系统健康": 7,
    "security": 8, "安全威胁": 8,
}


def normalize_level(value) -> str:
    """任意来源级别（'L4' / 'info' / 4 / '4' / None / 乱码）→ 规范桶名"""
    if value is None or isinstance(value, bool):
        return "info"
    if isinstance(value, (int, float)):
        n = int(value)
        return "L%d" % n if 1 <= n <= 5 else "info"
    s = str(value).strip().upper()
    if not s:
        return "info"
    if s.isdigit():
        n = int(s)
        return "L%d" % n if 1 <= n <= 5 else "info"
    if s.startswith("L") and s[1:].isdigit():
        n = int(s[1:])
        return "L%d" % n if 1 <= n <= 5 else "info"
    return "info"


def level_num(label: str) -> int:
    """规范桶名 → 数值级别（info=0）"""
    label = normalize_level(label)
    return int(label[1:]) if label != "info" else 0


def _parse_ts(value, text: str = "") -> int:
    """epoch 秒：优先数值 ts；其次解析 'YYYY-MM-DD HH:MM:SS'；失败 0（诚实：不猜）"""
    if isinstance(value, (int, float)) and value > 0:
        return int(value)
    for cand in (value, text):
        if not cand:
            continue
        s = str(cand).strip().replace("T", " ")
        if not s:
            continue
        for variant in (s, s[:19], s[:16], s[:10]):
            if not variant:
                continue
            for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
                try:
                    return int(time.mktime(time.strptime(variant, fmt)))
                except Exception:
                    continue
    return 0


def _read_record(path: str, day: str = "", fname: str = "") -> Optional[Dict]:
    """读单条预警 JSON → 规范记录；不可解析返回 None（不编造）"""
    try:
        with open(path, encoding="utf-8") as f:
            e = json.load(f)
    except Exception as ex:
        logger.debug("read alert record failed (%s): %s", path, ex)
        return None
    if not isinstance(e, dict):
        return None
    raw_level = e.get("level")
    if raw_level in (None, ""):
        raw_level = e.get("level_num")
    lv = normalize_level(raw_level)
    t_text = e.get("time") or (str(e.get("ts")) if e.get("ts") else "") or \
        ((day + " " + fname[4:14]) if day and fname else "")
    ts = _parse_ts(e.get("ts"), str(e.get("time") or ""))
    return {
        "title": e.get("title") or e.get("type_name") or "预警",
        "level": lv,
        "level_num": level_num(lv),
        "content": e.get("content") or e.get("message") or e.get("description") or "",
        "time": t_text,
        "ts": ts,
        "source": e.get("source", "alertd"),
        "type": e.get("type", 0),
        "type_name": e.get("type_name") or e.get("title") or "",
        "location": e.get("location", ""),
    }


def load_records(days: int = 14, per_day: int = 30, max_records: int = 50,
                 include_urgent: bool = True) -> List[Dict]:
    """读最近预警记录（新的在前；与 C 端目录/文件命名一致）"""
    records: List[Dict] = []
    if os.path.isdir(ALERT_BASE):
        day_dirs = [d for d in sorted(os.listdir(ALERT_BASE), reverse=True)
                    if d != "urgent" and os.path.isdir(os.path.join(ALERT_BASE, d))]
        for d in day_dirs[:max(1, int(days))]:
            dp = os.path.join(ALERT_BASE, d)
            try:
                files = [fn for fn in sorted(os.listdir(dp), reverse=True) if fn.endswith(".json")]
            except Exception as ex:
                logger.debug("list alert day dir failed (%s): %s", dp, ex)
                continue
            for fn in files[:max(1, int(per_day))]:
                rec = _read_record(os.path.join(dp, fn), day=d, fname=fn)
                if rec is not None:
                    records.append(rec)
                if len(records) >= max_records:
                    break
            if len(records) >= max_records:
                break

    if include_urgent and os.path.isdir(URGENT_DIR):
        urg: List[Dict] = []
        try:
            files = [fn for fn in sorted(os.listdir(URGENT_DIR), reverse=True) if fn.endswith(".json")]
        except Exception:
            files = []
        for fn in files[:10]:
            try:
                with open(os.path.join(URGENT_DIR, fn), encoding="utf-8") as f:
                    e = json.load(f)
            except Exception:
                continue
            if not isinstance(e, dict):
                continue
            urg.append({
                "title": e.get("title", "紧急"),
                "level": "L0",            # 紧急通道固定 L0（与命令面原语义一致）
                "level_num": 0,
                "content": e.get("content", ""),
                "time": e.get("time", fn[4:14]),
                "ts": _parse_ts(e.get("ts"), str(e.get("time") or "")),
                "source": e.get("source", "urgent"),
                "type": e.get("type", 0),
                "type_name": e.get("type_name", ""),
                "location": e.get("location", ""),
            })
        records = urg + records   # 紧急在前
    return records


def summarize(records: List[Dict]) -> Dict:
    """级别分布（info/L0-L5 完整）+ 源健康 + 近 7 日趋势 + 最严重级别"""
    levels = {k: 0 for k in LEVEL_ORDER}
    sources: Dict[str, Dict] = {}
    daily: Dict[str, int] = {}
    for r in records or []:
        if not isinstance(r, dict):
            continue
        lv = normalize_level(r.get("level"))
        levels[lv] = levels.get(lv, 0) + 1

        s = str(r.get("source") or "unknown")
        if s not in sources:
            sources[s] = {"count": 0, "last": r.get("time", "")}
        sources[s]["count"] += 1
        if r.get("time"):
            sources[s]["last"] = r.get("time", sources[s]["last"])

        d = str(r.get("time") or "")[:10]
        if len(d) == 10 and d[4] == "-":
            daily[d] = daily.get(d, 0) + 1

    trend = [{"date": d, "count": daily[d]} for d in sorted(daily.keys())[-7:]]
    highest = next((k for k in LEVEL_ORDER if k != "info" and levels.get(k, 0) > 0), "")
    return {
        "levels": levels,
        "level_order": list(LEVEL_ORDER),
        "highest_level": highest,
        "sources": sources,
        "trend": trend,
        "total": len(records or []),
    }


def filter_records(records: List[Dict], location: str = "", type_str: str = "",
                   hours: int = 24, now: Optional[int] = None) -> List[Dict]:
    """按地点/类型/时间窗过滤（供 alert_query 技能使用）

    · ts 不可解析（=0）的记录不参与时间过滤（信息保真，不误删）
    · type_str 支持英文/中文别名（台风/typhoon）与数字 type
    """
    now = int(now if now is not None else time.time())
    cutoff = now - int(hours) * 3600 if hours and hours > 0 else 0
    want_loc = str(location or "").strip().lower()
    want_type = str(type_str or "").strip().lower()
    want_tid = TYPE_ALIASES.get(want_type)

    out: List[Dict] = []
    for r in records or []:
        if not isinstance(r, dict):
            continue
        ts = int(r.get("ts") or 0)
        if cutoff and ts and ts < cutoff:
            continue
        if want_loc:
            hay = " ".join(str(r.get(k, "")) for k in
                           ("location", "title", "content", "type_name")).lower()
            if want_loc not in hay:
                continue
        if want_type:
            hay = " ".join(str(r.get(k, "")) for k in ("type_name", "title", "content")).lower()
            try:
                rec_tid = int(r.get("type") or -1)
            except Exception:
                rec_tid = -1
            if want_tid is not None:
                if rec_tid != want_tid and want_type not in hay:
                    continue
            elif want_type.isdigit():
                if rec_tid != int(want_type):
                    continue
            elif want_type not in hay:
                continue
        out.append(dict(r))
    return out
