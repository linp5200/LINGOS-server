#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 组织/体验扩展（先生 2026-09-12 裁决 · 批次5）

补齐 3 项缺失：
  M20 通知中心      —— 统一收件箱 + 历史 + 已读/免打扰
  M21 仪表盘自定义  —— 卡片布局（拖拽后的布局持久化）
  M22 媒体播放器控制 —— 对标 HA media_player（播放/暂停/音量/曲目）
"""

import json
import os
import time
import logging
import threading
import uuid
from typing import Dict, List, Optional

logger = logging.getLogger("UxExt")

CONFIG_PATH = "/LINGOS/system/config/ux_ext.json"
NOTIFY_DIR = "/LINGOS/data/notifications"

_lock = threading.RLock()
_notifications: List[dict] = []
_players: Dict[str, dict] = {}


# =============================================================
# 配置
# =============================================================
def _default_config() -> dict:
    return {
        "version": 1,
        "notify": {
            "max_items": 500,       # 通知历史上限
            "dnd": False,           # 免打扰
            "dnd_start": "23:00",
            "dnd_end": "07:00",
            "levels": ["info", "warn", "error", "critical"],
        },
        "dashboard": {
            "layouts": {},          # profile -> [卡片]
            "active": "default",
        },
        "media": {
            "players": {},          # entity_id -> 状态
        },
        "unread": 0,
    }


def _load() -> dict:
    with _lock:
        try:
            if os.path.exists(CONFIG_PATH):
                with open(CONFIG_PATH, encoding="utf-8") as f:
                    d = json.load(f)
                base = _default_config()
                for k, v in base.items():
                    d.setdefault(k, v)
                return d
        except Exception as e:
            logger.warning("load ux config failed: %s", e)
        return _default_config()


def _save(cfg: dict) -> None:
    with _lock:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        tmp = CONFIG_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        os.replace(tmp, CONFIG_PATH)


# =============================================================
# M20 · 通知中心
# =============================================================
def _in_dnd(cfg: dict) -> bool:
    n = cfg.get("notify", {})
    if not n.get("dnd"):
        return False
    def _hm(s, dflt):
        try:
            h, m = str(s).split(":")
            return int(h) * 60 + int(m)
        except Exception:
            return dflt
    now = time.localtime()
    cur = now.tm_hour * 60 + now.tm_min
    s = _hm(n.get("dnd_start"), 23 * 60)
    e = _hm(n.get("dnd_end"), 7 * 60)
    return (cur >= s or cur < e) if s > e else (s <= cur < e)


def cmd_notify_push(title: str = "", body: str = "", level: str = "info",
                    source: str = "", url: str = "") -> dict:
    """推送一条通知到中心（同时尝试系统通知）"""
    cfg = _load()
    n = cfg.setdefault("notify", {})
    lv = level if level in n.get("levels", []) else "info"
    item = {
        "id": uuid.uuid4().hex[:10],
        "title": title or "通知",
        "body": body or "",
        "level": lv,
        "source": source or "system",
        "url": url or "",
        "ts": int(time.time()),
        "read": False,
    }
    # 免打扰：critical 仍显示，其余标记为静默
    item["silenced"] = _in_dnd(cfg) and lv != "critical"

    with _lock:
        _notifications.insert(0, item)
        if len(_notifications) > int(n.get("max_items", 500)):
            del _notifications[int(n.get("max_items", 500)):]
        cfg["unread"] = sum(1 for x in _notifications if not x.get("read"))
        _save(cfg)

    # 顺带尝试系统通知（失败不影响中心记录）
    if not item["silenced"]:
        try:
            from syscall_client import call_syscall
            call_syscall("notify", {"title": item["title"], "body": item["body"]}, timeout=3)
        except Exception as e:
            logger.debug("system notify failed: %s", e)

    # 【0.7.0-hf2】WS 实时广播（此前缺失——App 只能轮询，无法实时收通知！
    #   现在：前台立即呈现 + 后台由 App 通知桥转本地通知）
    if not item["silenced"]:
        try:
            from ai_server import _broadcast_alert_event
            _broadcast_alert_event({"type": "notify_event", "data": {
                "id": item["id"], "title": item["title"], "body": item["body"],
                "level": lv, "source": item["source"], "ts": item["ts"]}})
        except Exception as e:
            logger.debug("notify ws broadcast failed: %s", e)

    logger.info("notify push [%s] %s (silenced=%s)", lv, item["title"], item["silenced"])
    return {"status": "ok", "data": item}


def cmd_notify_list(limit: int = 50, unread_only: bool = False,
                    level: str = "") -> dict:
    cfg = _load()
    items = list(_notifications)
    if unread_only:
        items = [x for x in items if not x.get("read")]
    if level:
        items = [x for x in items if x.get("level") == level]
    return {"status": "ok", "data": {
        "items": items[:max(1, min(limit, 500))],
        "total": len(_notifications),
        "unread": cfg.get("unread", 0),
        "dnd": bool(cfg.get("notify", {}).get("dnd")),
    }}


def cmd_notify_mark_read(notify_id: str = "", all_items: bool = False) -> dict:
    cfg = _load()
    n = 0
    with _lock:
        for x in _notifications:
            if all_items or x.get("id") == notify_id:
                if not x.get("read"):
                    x["read"] = True
                    n += 1
        cfg["unread"] = sum(1 for x in _notifications if not x.get("read"))
        _save(cfg)
    return {"status": "ok", "data": {"marked": n, "unread": cfg.get("unread", 0)}}


def cmd_notify_clear() -> dict:
    cfg = _load()
    with _lock:
        cnt = len(_notifications)
        _notifications.clear()
        cfg["unread"] = 0
        _save(cfg)
    return {"status": "ok", "data": {"cleared": cnt}}


def cmd_notify_dnd(enabled: bool = None, start: str = "", end: str = "") -> dict:
    cfg = _load()
    n = cfg.setdefault("notify", {})
    if enabled is not None:
        n["dnd"] = bool(enabled)
    if start:
        n["dnd_start"] = start
    if end:
        n["dnd_end"] = end
    _save(cfg)
    return {"status": "ok", "data": {"dnd": n.get("dnd"), "start": n.get("dnd_start"),
                                     "end": n.get("dnd_end")}}


# =============================================================
# M21 · 仪表盘自定义
# =============================================================
DEFAULT_CARDS = [
    {"id": "sys", "type": "system", "title": "系统", "span": 1, "order": 0},
    {"id": "alert", "type": "alerts", "title": "预警", "span": 1, "order": 1},
    {"id": "weather", "type": "weather", "title": "天气", "span": 1, "order": 2},
    {"id": "ha", "type": "home", "title": "智能家居", "span": 1, "order": 3},
    {"id": "notify", "type": "notifications", "title": "通知", "span": 1, "order": 4},
    {"id": "media", "type": "media", "title": "媒体", "span": 1, "order": 5},
]


def cmd_dashboard_get(profile: str = "default") -> dict:
    cfg = _load()
    d = cfg.setdefault("dashboard", {"layouts": {}, "active": "default"})
    layout = d.get("layouts", {}).get(profile)
    if not layout:
        layout = list(DEFAULT_CARDS)
    return {"status": "ok", "data": {"profile": profile, "cards": layout,
                                     "active": d.get("active", "default"),
                                     "profiles": list(d.get("layouts", {}).keys())}}


def cmd_dashboard_set(cards: str = "", profile: str = "default") -> dict:
    """cards: JSON 数组 [{"id":"sys","type":"system","title":"系统","span":1,"order":0}]"""
    try:
        layout = json.loads(cards) if cards else []
    except Exception:
        return {"status": "error", "msg": "cards 必须是 JSON 数组"}
    if not isinstance(layout, list):
        return {"status": "error", "msg": "cards 必须是数组"}
    # 规范化
    norm = []
    for i, c in enumerate(layout):
        if not isinstance(c, dict):
            continue
        norm.append({
            "id": c.get("id") or uuid.uuid4().hex[:6],
            "type": c.get("type", "custom"),
            "title": c.get("title", ""),
            "span": max(1, min(4, int(c.get("span", 1)))),
            "order": int(c.get("order", i)),
            "config": c.get("config", {}),
        })
    norm.sort(key=lambda x: x["order"])
    cfg = _load()
    cfg.setdefault("dashboard", {}).setdefault("layouts", {})[profile] = norm
    cfg["dashboard"]["active"] = profile
    _save(cfg)
    return {"status": "ok", "data": {"profile": profile, "cards": norm}}


def cmd_dashboard_reset(profile: str = "default") -> dict:
    cfg = _load()
    cfg.setdefault("dashboard", {}).setdefault("layouts", {})[profile] = list(DEFAULT_CARDS)
    _save(cfg)
    return {"status": "ok", "data": {"profile": profile, "cards": list(DEFAULT_CARDS)}}


def cmd_dashboard_add_card(card_type: str = "", title: str = "", span: int = 1,
                           profile: str = "default", config: str = "") -> dict:
    cfg = _load()
    d = cfg.setdefault("dashboard", {"layouts": {}, "active": "default"})
    layout = d.setdefault("layouts", {}).setdefault(profile, list(DEFAULT_CARDS))
    try:
        ccfg = json.loads(config) if config else {}
    except Exception:
        ccfg = {}
    card = {
        "id": uuid.uuid4().hex[:6], "type": card_type or "custom",
        "title": title or card_type, "span": max(1, min(4, int(span))),
        "order": len(layout), "config": ccfg,
    }
    layout.append(card)
    _save(cfg)
    return {"status": "ok", "data": card}


def cmd_dashboard_remove_card(card_id: str = "", profile: str = "default") -> dict:
    cfg = _load()
    d = cfg.setdefault("dashboard", {"layouts": {}})
    layout = d.get("layouts", {}).get(profile, [])
    before = len(layout)
    d["layouts"][profile] = [c for c in layout if c.get("id") != card_id]
    _save(cfg)
    return {"status": "ok", "data": {"removed": before - len(d["layouts"][profile])}}


# =============================================================
# M22 · 媒体播放器控制（对标 HA media_player）
# =============================================================
def _player(entity_id: str) -> dict:
    return _players.setdefault(entity_id, {
        "entity_id": entity_id,
        "state": "idle",          # idle|playing|paused|buffering|off
        "volume": 0.5,
        "muted": False,
        "media_title": "",
        "media_artist": "",
        "media_album": "",
        "media_duration": 0,
        "media_position": 0,
        "source": "",
        "updated": int(time.time()),
    })


def cmd_media_register(entity_id: str = "", name: str = "") -> dict:
    if not entity_id:
        return {"status": "error", "msg": "缺少 entity_id"}
    p = _player(entity_id)
    if name:
        p["name"] = name
    p["updated"] = int(time.time())
    return {"status": "ok", "data": p}


def cmd_media_list() -> dict:
    return {"status": "ok", "data": list(_players.values())}


def cmd_media_command(entity_id: str = "", command: str = "",
                      value: str = "") -> dict:
    """command: play|pause|stop|next|previous|volume_set|volume_up|volume_down|
                mute|unmute|play_media|seek"""
    if not entity_id or not command:
        return {"status": "error", "msg": "缺少 entity_id 或 command"}
    p = _player(entity_id)

    if command == "play":
        p["state"] = "playing"
    elif command == "pause":
        p["state"] = "paused"
    elif command == "stop":
        p["state"] = "idle"; p["media_position"] = 0
    elif command == "next":
        p["media_position"] = 0; p["state"] = "playing"
    elif command == "previous":
        p["media_position"] = 0; p["state"] = "playing"
    elif command == "volume_set":
        try:
            p["volume"] = max(0.0, min(1.0, float(value)))
        except Exception:
            return {"status": "error", "msg": "value 必须为 0.0~1.0"}
    elif command == "volume_up":
        p["volume"] = min(1.0, p["volume"] + 0.05)
    elif command == "volume_down":
        p["volume"] = max(0.0, p["volume"] - 0.05)
    elif command == "mute":
        p["muted"] = True
    elif command == "unmute":
        p["muted"] = False
    elif command == "play_media":
        # value: JSON {"title":..,"artist":..,"url":..,"duration":..}
        try:
            info = json.loads(value) if value else {}
        except Exception:
            info = {"title": value}
        p["media_title"] = info.get("title", value)
        p["media_artist"] = info.get("artist", "")
        p["media_album"] = info.get("album", "")
        p["media_duration"] = int(info.get("duration", 0) or 0)
        p["source"] = info.get("url", "")
        p["media_position"] = 0
        p["state"] = "playing"
    elif command == "seek":
        try:
            p["media_position"] = max(0, int(float(value)))
        except Exception:
            return {"status": "error", "msg": "value 必须为秒数"}
    else:
        return {"status": "error", "msg": "未知命令: %s" % command}

    p["updated"] = int(time.time())
    logger.debug("media %s → %s", entity_id, command)
    return {"status": "ok", "data": p}


def cmd_media_state(entity_id: str = "") -> dict:
    if not entity_id:
        return {"status": "error", "msg": "缺少 entity_id"}
    return {"status": "ok", "data": _player(entity_id)}


# =============================================================
# 总览
# =============================================================
def cmd_ux_overview() -> dict:
    cfg = _load()
    return {"status": "ok", "data": {
        "notifications": len(_notifications),
        "unread": cfg.get("unread", 0),
        "dnd": bool(cfg.get("notify", {}).get("dnd")),
        "dashboard_profiles": len(cfg.get("dashboard", {}).get("layouts", {})),
        "dashboard_cards": len(cmd_dashboard_get()["data"]["cards"]),
        "players": len(_players),
        "playing": sum(1 for p in _players.values() if p.get("state") == "playing"),
    }}
