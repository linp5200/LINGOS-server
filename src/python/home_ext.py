#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 智能家居扩展（先生 2026-09-12 裁决 · 批次2）

对标 Home Assistant 核心能力，补齐 8 项缺失：
  M1 设备自动发现   —— mDNS/SSDP/Zeroconf + MQTT Discovery
  M2 场景 Scene     —— 多实体状态快照/恢复
  M3 区域 Area + 标签 Label —— 设备组织
  M4 能源管理       —— 功率/电量/成本统计
  M5 在家/离家 Presence —— 状态机 + 触发
  M6 地理围栏 Zone  —— 位置触发
  M7 Webhook        —— 外部触发自动化
  M8 蓝图 Blueprint —— 自动化模板

存储：/LINGOS/system/config/home_ext.json（中敏）
与 HA 的关系：**本模块可独立工作**（本地实体），也可桥接 HA（复用 ha_integration）
"""

import json
import os
import time
import logging
import threading
import uuid
import socket
import struct
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger("HomeExt")

CONFIG_PATH = "/LINGOS/system/config/home_ext.json"
DATA_DIR = "/LINGOS/data/home"

_lock = threading.RLock()
_state_cache: Dict[str, dict] = {}      # entity_id -> 最近状态
_energy_log: List[dict] = []            # 能源采样
_presence = {"state": "unknown", "since": 0, "source": ""}


# =============================================================
# 【2026-09-18 接线】HA 实体控制桥（方案2 §1.3 设计一——治「孤岛」）
#   此前 entity_set_state / scene_apply 只写内存 cache（HA 里的真设备纹丝不动）
#   现：HA 已配置 → 真实 service call；未配置 → 本地缓存 + 明确标注
# =============================================================
_STATE_SERVICE = {
    "on": "turn_on", "off": "turn_off",
    "open": "open_cover", "closed": "close_cover",
    "lock": "lock", "unlock": "unlock",
    "playing": "media_play", "paused": "media_pause",
    "home": "home", "not_home": "not_home",
}
# 纯本地域（无 HA 对应）——不尝试桥接
_LOCAL_DOMAINS = {"virtual", "scene", "automation", "script"}
# 高风险域（桥接时走 ha_control 高风险检查——开锁等需确认）
_HIGH_RISK_DOMAINS = {"lock"}


def _entity_domain(entity_id: str) -> str:
    return entity_id.split(".", 1)[0] if "." in entity_id else ""


def _ha_bridge_control(entity_id: str, state: str) -> dict:
    """实体控制桥：HA 已配置 → service call；否则 {"bridged": False, "reason": ...}"""
    domain = _entity_domain(entity_id)
    if not domain or domain in _LOCAL_DOMAINS:
        return {"bridged": False, "reason": "local_domain"}
    try:
        import ha_integration as HA
    except Exception:
        return {"bridged": False, "reason": "ha_module_unavailable"}
    try:
        cfg = HA.ha_load_config()
        if not cfg.get("host"):
            return {"bridged": False, "reason": "ha_not_configured"}
        service = _STATE_SERVICE.get(str(state).lower(), str(state).lower())
        # 高风险域（锁等）→ 用 need_confirm 版（危机之外的常规路径不静默开锁）
        if domain in _HIGH_RISK_DOMAINS:
            r = HA.cmd_ha_control(domain, service, entity_id, "")
            return {"bridged": True, "service": service, "result": r,
                    "note": "high-risk domain — confirmation flow applies"}
        r = HA._ha_do_control(cfg, domain, service, entity_id, "")
        return {"bridged": True, "service": service, "result": r}
    except Exception as e:
        logger.debug("ha bridge failed: %s", e)
        return {"bridged": False, "reason": "bridge_error", "msg": str(e)}

# =============================================================
# 存储
# =============================================================
def _default_config() -> dict:
    return {
        "version": 1,
        "areas": [],          # [{id, name, icon, parent}]
        "labels": [],         # [{id, name, color}]
        "entities": [],       # [{id, name, domain, area, labels[], state}]
        "scenes": [],         # [{id, name, entities:[{id, state}]}]
        "zones": [],          # [{id, name, lat, lon, radius, trigger}]
        "webhooks": [],       # [{id, name, secret, actions[]}]
        "blueprints": [],     # [{id, name, params[], steps[]}]
        "automations": [],    # [{id, name, trigger, condition, actions, enabled}]
        "energy": {"samples": [], "tariff": 0.55, "currency": "CNY"},
        "discovery": {"enabled": True, "found": [], "last_scan": 0},
        "presence": {"state": "unknown", "since": 0},
    }


def _load() -> dict:
    with _lock:
        try:
            if os.path.exists(CONFIG_PATH):
                with open(CONFIG_PATH, encoding="utf-8") as f:
                    d = json.load(f)
                # 合并缺失键（向后兼容）
                base = _default_config()
                for k, v in base.items():
                    d.setdefault(k, v)
                return d
        except Exception as e:
            logger.warning("load config failed: %s", e)
        return _default_config()


def _save(cfg: dict) -> None:
    with _lock:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        tmp = CONFIG_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        os.replace(tmp, CONFIG_PATH)


# =============================================================
# M1 · 设备自动发现（mDNS / SSDP / MQTT Discovery）
# =============================================================
def _discover_mdns(timeout: float = 3.0) -> List[dict]:
    """mDNS (224.0.0.251:5353) 查询 _services._dns-sd._udp.local

    简化实现：发送 PTR 查询并收集应答中的服务类型/主机名。
    """
    found = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.settimeout(timeout)
        # 构造 mDNS 查询：_services._dns-sd._udp.local PTR
        q = b"\x00\x00"          # ID
        q += b"\x00\x00"         # Flags (standard query)
        q += b"\x00\x01"         # QDCOUNT
        q += b"\x00\x00\x00\x00\x00\x00"  # AN/NS/AR
        # _services._dns-sd._udp.local
        for part in [b"_services", b"_dns-sd", b"_udp", b"local"]:
            q += bytes([len(part)]) + part
        q += b"\x00"             # root
        q += b"\x00\x0c"         # PTR
        q += b"\x00\x01"         # IN
        try:
            s.sendto(q, ("224.0.0.251", 5353))
        except Exception as e:
            logger.debug("mdns send failed: %s", e)
            s.close()
            return found
        deadline = time.time() + timeout
        seen = set()
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                break
            except Exception:
                break
            host = addr[0]
            # 从报文中粗略提取可读服务名
            txt = "".join(chr(b) if 32 <= b < 127 else "." for b in data[:120])
            for token in txt.split("."):
                if token.startswith("_") and len(token) > 2 and token not in seen:
                    seen.add(token)
                    found.append({"type": "mdns", "service": token,
                                  "host": host, "raw": addr[1]})
        s.close()
    except Exception as e:
        logger.debug("mdns discovery failed: %s", e)
    return found


def _discover_ssdp(timeout: float = 3.0) -> List[dict]:
    """SSDP (239.255.255.250:1900) M-SEARCH 发现 UPnP 设备"""
    found = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.settimeout(timeout)
        msg = (
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            'MAN: "ssdp:discover"\r\n'
            "MX: 2\r\n"
            "ST: ssdp:all\r\n\r\n"
        ).encode()
        s.sendto(msg, ("239.255.255.250", 1900))
        deadline = time.time() + timeout
        seen = set()
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                break
            except Exception:
                break
            text = data.decode("utf-8", "ignore")
            info = {"type": "ssdp", "host": addr[0]}
            for line in text.split("\r\n"):
                low = line.lower()
                if low.startswith("server:"):
                    info["server"] = line.split(":", 1)[1].strip()
                elif low.startswith("st:"):
                    info["st"] = line.split(":", 1)[1].strip()
                elif low.startswith("location:"):
                    info["location"] = line.split(":", 1)[1].strip()
            key = (info.get("host"), info.get("st", ""))
            if key not in seen:
                seen.add(key)
                found.append(info)
        s.close()
    except Exception as e:
        logger.debug("ssdp discovery failed: %s", e)
    return found


def cmd_discovery_scan(timeout: float = 3.0) -> dict:
    """扫描局域网设备（mDNS + SSDP + MQTT Discovery 记录）"""
    cfg = _load()
    if not cfg.get("discovery", {}).get("enabled", True):
        return {"status": "error", "msg": "设备发现已关闭（可在选项中开启）"}

    mdns = _discover_mdns(timeout)
    ssdp = _discover_ssdp(timeout)

    # 合并 MQTT Discovery 已知项（由 ha/mqtt 模块上报）
    known = cfg.get("discovery", {}).get("found", [])
    merged = {(x.get("host"), x.get("st") or x.get("service")): x for x in known}
    for x in mdns + ssdp:
        merged[(x.get("host"), x.get("st") or x.get("service"))] = x
    result = list(merged.values())

    cfg.setdefault("discovery", {})["found"] = result
    cfg["discovery"]["last_scan"] = int(time.time())
    _save(cfg)

    logger.info("discovery scan: mdns=%d ssdp=%d total=%d", len(mdns), len(ssdp), len(result))
    return {"status": "ok", "data": {
        "mdns": mdns, "ssdp": ssdp, "total": len(result),
        "scanned_at": cfg["discovery"]["last_scan"],
    }}


def cmd_discovery_list() -> dict:
    cfg = _load()
    d = cfg.get("discovery", {})
    return {"status": "ok", "data": {
        "enabled": d.get("enabled", True),
        "found": d.get("found", []),
        "last_scan": d.get("last_scan", 0),
    }}


def cmd_discovery_set(enabled: bool = True) -> dict:
    cfg = _load()
    cfg.setdefault("discovery", {})["enabled"] = bool(enabled)
    _save(cfg)
    return {"status": "ok", "data": {"enabled": bool(enabled)}}


# =============================================================
# M3 · 区域 / 标签 / 实体
# =============================================================
def cmd_area_list() -> dict:
    return {"status": "ok", "data": _load().get("areas", [])}


def cmd_area_add(name: str = "", icon: str = "", parent: str = "") -> dict:
    if not name:
        return {"status": "error", "msg": "缺少 name"}
    cfg = _load()
    aid = uuid.uuid4().hex[:8]
    cfg.setdefault("areas", []).append(
        {"id": aid, "name": name, "icon": icon or "home", "parent": parent})
    _save(cfg)
    return {"status": "ok", "data": {"id": aid}}


def cmd_area_remove(area_id: str = "") -> dict:
    cfg = _load()
    before = len(cfg.get("areas", []))
    cfg["areas"] = [a for a in cfg.get("areas", []) if a.get("id") != area_id]
    # 解除实体关联
    for e in cfg.get("entities", []):
        if e.get("area") == area_id:
            e["area"] = ""
    _save(cfg)
    return {"status": "ok", "data": {"removed": before - len(cfg["areas"])}}


def cmd_label_list() -> dict:
    return {"status": "ok", "data": _load().get("labels", [])}


def cmd_label_add(name: str = "", color: str = "") -> dict:
    if not name:
        return {"status": "error", "msg": "缺少 name"}
    cfg = _load()
    lid = uuid.uuid4().hex[:8]
    cfg.setdefault("labels", []).append({"id": lid, "name": name, "color": color or ""})
    _save(cfg)
    return {"status": "ok", "data": {"id": lid}}


def cmd_entity_list(area: str = "", label: str = "") -> dict:
    ents = _load().get("entities", [])
    if area:
        ents = [e for e in ents if e.get("area") == area]
    if label:
        ents = [e for e in ents if label in (e.get("labels") or [])]
    # 附加实时状态
    for e in ents:
        e["live_state"] = _state_cache.get(e.get("id"), {}).get("state")
    return {"status": "ok", "data": ents}


def cmd_entity_add(entity_id: str = "", name: str = "", domain: str = "",
                   area: str = "", labels: str = "") -> dict:
    if not entity_id:
        return {"status": "error", "msg": "缺少 entity_id"}
    cfg = _load()
    lbl = [x.strip() for x in (labels or "").split(",") if x.strip()]
    cfg.setdefault("entities", []).append({
        "id": entity_id, "name": name or entity_id,
        "domain": domain or entity_id.split(".")[0],
        "area": area, "labels": lbl, "state": "unknown",
    })
    _save(cfg)
    return {"status": "ok", "data": {"id": entity_id}}


def cmd_entity_set_state(entity_id: str = "", state: str = "", **attrs) -> dict:
    if not entity_id:
        return {"status": "error", "msg": "缺少 entity_id"}
    _state_cache[entity_id] = {"state": state, "attrs": attrs, "ts": int(time.time())}
    # 【2026-09-18 接线】HA 桥：已配置 HA → 真实设备 service call（此前只写内存——孤岛修复）
    bridge = _ha_bridge_control(entity_id, state)
    # 触发自动化
    try:
        _run_automations("state_changed", {"entity_id": entity_id, "state": state})
    except Exception as e:
        logger.debug("automation trigger failed: %s", e)
    data = dict(_state_cache[entity_id])
    data["ha_bridge"] = bridge
    return {"status": "ok", "data": data}


# =============================================================
# M2 · 场景 Scene
# =============================================================
def cmd_scene_list() -> dict:
    return {"status": "ok", "data": _load().get("scenes", [])}


def cmd_scene_create(name: str = "", entities: str = "") -> dict:
    """entities: JSON 字符串 [{"id":"light.x","state":"on"}] 或逗号分隔 id 列表（取当前态）"""
    cfg = _load()
    sid = uuid.uuid4().hex[:8]
    snap = []
    if entities:
        try:
            parsed = json.loads(entities)
            if isinstance(parsed, list):
                for it in parsed:
                    if isinstance(it, dict) and it.get("id"):
                        snap.append({"id": it["id"], "state": it.get("state", "on")})
                    elif isinstance(it, str):
                        cur = _state_cache.get(it, {}).get("state", "on")
                        snap.append({"id": it, "state": cur})
        except Exception:
            for eid in [x.strip() for x in entities.split(",") if x.strip()]:
                cur = _state_cache.get(eid, {}).get("state", "on")
                snap.append({"id": eid, "state": cur})
    cfg.setdefault("scenes", []).append({"id": sid, "name": name or ("场景 " + sid), "entities": snap})
    _save(cfg)
    return {"status": "ok", "data": {"id": sid, "entities": snap}}


def cmd_scene_apply(scene_id: str = "") -> dict:
    cfg = _load()
    sc = next((s for s in cfg.get("scenes", []) if s.get("id") == scene_id), None)
    if not sc:
        return {"status": "error", "msg": "场景不存在"}
    applied = []
    for it in sc.get("entities", []):
        eid = it.get("id")
        st = it.get("state")
        _state_cache[eid] = {"state": st, "ts": int(time.time()), "scene": sc.get("name")}
        # 【2026-09-18 接线】场景实体经 HA 桥落地真设备（此前只动内存）
        entry = {"id": eid, "state": st}
        try:
            br = _ha_bridge_control(eid, st)
            if br.get("bridged"):
                r = br.get("result", {})
                entry["ha"] = r.get("status", "ok") if isinstance(r, dict) else "ok"
        except Exception as e:
            entry["ha"] = "bridge_error: %s" % e
        applied.append(entry)
    try:
        _run_automations("scene_applied", {"scene": sc.get("name"), "entities": applied})
    except Exception:
        pass
    return {"status": "ok", "data": {"applied": applied}}


# =============================================================
# M4 · 能源管理
# =============================================================
def cmd_energy_add(entity_id: str = "", power: float = 0.0,
                   energy: float = 0.0) -> dict:
    """记录一次能源采样（power=瞬时功率 W，energy=累计电量 kWh）"""
    cfg = _load()
    e = cfg.setdefault("energy", {"samples": [], "tariff": 0.55, "currency": "CNY"})
    e.setdefault("samples", []).append({
        "entity": entity_id, "power_w": float(power),
        "energy_kwh": float(energy), "ts": int(time.time()),
    })
    # 保留最近 2000 条
    if len(e["samples"]) > 2000:
        e["samples"] = e["samples"][-2000:]
    _save(cfg)
    return {"status": "ok"}


def cmd_energy_summary(hours: int = 24) -> dict:
    cfg = _load()
    e = cfg.get("energy", {})
    since = int(time.time()) - max(1, hours) * 3600
    samples = [s for s in e.get("samples", []) if s.get("ts", 0) >= since]

    by_entity: Dict[str, dict] = {}
    for s in samples:
        ent = s.get("entity") or "unknown"
        d = by_entity.setdefault(ent, {"power_w": 0.0, "energy_kwh": 0.0, "count": 0})
        d["power_w"] += s.get("power_w", 0.0)
        d["energy_kwh"] += s.get("energy_kwh", 0.0)
        d["count"] += 1
    for d in by_entity.values():
        if d["count"]:
            d["power_w"] = round(d["power_w"] / d["count"], 2)

    total_kwh = round(sum(d["energy_kwh"] for d in by_entity.values()), 3)
    tariff = e.get("tariff", 0.55)
    return {"status": "ok", "data": {
        "window_hours": hours,
        "total_energy_kwh": total_kwh,
        "total_cost": round(total_kwh * tariff, 2),
        "currency": e.get("currency", "CNY"),
        "tariff": tariff,
        "by_entity": by_entity,
        "samples": len(samples),
    }}


def cmd_energy_set_tariff(tariff: float = 0.55, currency: str = "CNY") -> dict:
    cfg = _load()
    e = cfg.setdefault("energy", {})
    e["tariff"] = float(tariff)
    e["currency"] = currency or "CNY"
    _save(cfg)
    return {"status": "ok", "data": {"tariff": e["tariff"], "currency": e["currency"]}}


# =============================================================
# M5 · 在家/离家 Presence
# =============================================================
def cmd_presence_get() -> dict:
    cfg = _load()
    p = cfg.get("presence", {})
    return {"status": "ok", "data": {"state": p.get("state", "unknown"),
                                     "since": p.get("since", 0),
                                     "source": p.get("source", "")}}


def cmd_presence_set(state: str = "", source: str = "") -> dict:
    if state not in ("home", "away", "unknown"):
        return {"status": "error", "msg": "state 必须是 home/away/unknown"}
    cfg = _load()
    old = cfg.get("presence", {}).get("state", "unknown")
    cfg["presence"] = {"state": state, "since": int(time.time()), "source": source or "manual"}
    _save(cfg)
    if old != state:
        try:
            _run_automations("presence_changed", {"from": old, "to": state})
        except Exception:
            pass
    logger.info("presence: %s → %s (%s)", old, state, source or "manual")
    return {"status": "ok", "data": cfg["presence"]}


# =============================================================
# M6 · 地理围栏 Zone
# =============================================================
def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    import math
    R = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.asin(min(1.0, a ** 0.5))


def cmd_zone_list() -> dict:
    return {"status": "ok", "data": _load().get("zones", [])}


def cmd_zone_add(name: str = "", lat: float = 0.0, lon: float = 0.0,
                 radius: float = 100.0, trigger: str = "enter") -> dict:
    if not name:
        return {"status": "error", "msg": "缺少 name"}
    cfg = _load()
    zid = uuid.uuid4().hex[:8]
    cfg.setdefault("zones", []).append({
        "id": zid, "name": name, "lat": float(lat), "lon": float(lon),
        "radius": float(radius), "trigger": trigger or "enter",
        "inside": False,
    })
    _save(cfg)
    return {"status": "ok", "data": {"id": zid}}


def cmd_zone_remove(zone_id: str = "") -> dict:
    cfg = _load()
    before = len(cfg.get("zones", []))
    cfg["zones"] = [z for z in cfg.get("zones", []) if z.get("id") != zone_id]
    _save(cfg)
    return {"status": "ok", "data": {"removed": before - len(cfg["zones"])}}


def cmd_zone_check(lat: float = 0.0, lon: float = 0.0) -> dict:
    """检查当前位置与各围栏关系，触发 enter/leave"""
    cfg = _load()
    hits = []
    for z in cfg.get("zones", []):
        d = _haversine_m(lat, lon, z.get("lat", 0.0), z.get("lon", 0.0))
        inside = d <= z.get("radius", 100.0)
        was = bool(z.get("inside", False))
        z["inside"] = inside
        if inside != was:
            evt = "enter" if inside else "leave"
            hits.append({"zone": z.get("name"), "event": evt, "distance_m": round(d, 1)})
            try:
                _run_automations("zone_" + evt,
                                 {"zone": z.get("name"), "lat": lat, "lon": lon})
            except Exception:
                pass
    _save(cfg)
    return {"status": "ok", "data": {"crossings": hits}}


# =============================================================
# M7 · Webhook
# =============================================================
def cmd_webhook_list() -> dict:
    cfg = _load()
    whs = [{"id": w.get("id"), "name": w.get("name"),
            "path": "/api/webhook/" + str(w.get("id")),
            "actions": len(w.get("actions", []))}
           for w in cfg.get("webhooks", [])]
    return {"status": "ok", "data": whs}


def cmd_webhook_add(name: str = "", actions: str = "") -> dict:
    """actions: JSON 字符串 [{"type":"scene","id":"xxx"}] 或 [{"type":"presence","state":"home"}]"""
    cfg = _load()
    wid = uuid.uuid4().hex[:8]
    acts = []
    if actions:
        try:
            parsed = json.loads(actions)
            if isinstance(parsed, list):
                acts = parsed
        except Exception:
            pass
    cfg.setdefault("webhooks", []).append({
        "id": wid, "name": name or ("Webhook " + wid), "actions": acts,
        "created": int(time.time()),
    })
    _save(cfg)
    return {"status": "ok", "data": {"id": wid, "path": "/api/webhook/" + wid}}


def cmd_webhook_remove(webhook_id: str = "") -> dict:
    cfg = _load()
    before = len(cfg.get("webhooks", []))
    cfg["webhooks"] = [w for w in cfg.get("webhooks", []) if w.get("id") != webhook_id]
    _save(cfg)
    return {"status": "ok", "data": {"removed": before - len(cfg["webhooks"])}}


def cmd_webhook_trigger(webhook_id: str = "", payload: str = "") -> dict:
    """外部系统调用：执行该 webhook 绑定的动作"""
    cfg = _load()
    wh = next((w for w in cfg.get("webhooks", []) if w.get("id") == webhook_id), None)
    if not wh:
        return {"status": "error", "msg": "webhook 不存在"}
    results = []
    for act in wh.get("actions", []):
        t = act.get("type")
        try:
            if t == "scene":
                results.append({"action": "scene", "result": cmd_scene_apply(act.get("id", ""))})
            elif t == "presence":
                results.append({"action": "presence",
                                "result": cmd_presence_set(act.get("state", "home"), "webhook")})
            elif t == "notification":
                try:
                    from syscall_client import call_syscall
                    call_syscall("notify", {"title": act.get("title", "Webhook"),
                                            "body": act.get("body", payload)}, timeout=5)
                except Exception:
                    pass
                results.append({"action": "notification", "result": "sent"})
            else:
                results.append({"action": t, "result": "unknown action type"})
        except Exception as e:
            results.append({"action": t, "result": "error: %s" % e})
    logger.info("webhook '%s' triggered, %d actions", wh.get("name"), len(results))
    return {"status": "ok", "data": {"webhook": wh.get("name"), "results": results}}


# =============================================================
# M8 · 蓝图 Blueprint（自动化模板）
# =============================================================
def cmd_blueprint_list() -> dict:
    return {"status": "ok", "data": _load().get("blueprints", [])}


def cmd_blueprint_add(name: str = "", params: str = "", steps: str = "") -> dict:
    """params: ["target_entity","scene_id"]，steps: [{"type":"scene","id":"${scene_id}"}]"""
    cfg = _load()
    bid = uuid.uuid4().hex[:8]
    try:
        p = json.loads(params) if params else []
    except Exception:
        p = [x.strip() for x in (params or "").split(",") if x.strip()]
    try:
        st = json.loads(steps) if steps else []
    except Exception:
        st = []
    cfg.setdefault("blueprints", []).append({
        "id": bid, "name": name or ("蓝图 " + bid), "params": p, "steps": st})
    _save(cfg)
    return {"status": "ok", "data": {"id": bid}}


def cmd_blueprint_apply(blueprint_id: str = "", values: str = "") -> dict:
    """values: JSON {"scene_id":"abc"} —— 替换步骤中的 ${param} 占位符"""
    cfg = _load()
    bp = next((b for b in cfg.get("blueprints", []) if b.get("id") == blueprint_id), None)
    if not bp:
        return {"status": "error", "msg": "蓝图不存在"}
    try:
        vals = json.loads(values) if values else {}
    except Exception:
        vals = {}
    def _sub(o):
        if isinstance(o, str):
            for k, v in vals.items():
                o = o.replace("${%s}" % k, str(v))
            return o
        if isinstance(o, dict):
            return {k: _sub(v) for k, v in o.items()}
        if isinstance(o, list):
            return [_sub(x) for x in o]
        return o
    results = []
    for step in _sub(bp.get("steps", [])):
        t = step.get("type")
        if t == "scene":
            results.append(cmd_scene_apply(step.get("id", "")))
        elif t == "presence":
            results.append(cmd_presence_set(step.get("state", "home"), "blueprint"))
        elif t == "delay":
            time.sleep(min(60, int(step.get("seconds", 1))))
            results.append({"delay": step.get("seconds")})
    return {"status": "ok", "data": {"blueprint": bp.get("name"), "results": results}}


# =============================================================
# 自动化（触发器 → 条件 → 动作）
# =============================================================
def cmd_automation_list() -> dict:
    return {"status": "ok", "data": _load().get("automations", [])}


def cmd_automation_add(name: str = "", trigger: str = "state_changed",
                       condition: str = "", actions: str = "") -> dict:
    cfg = _load()
    aid = uuid.uuid4().hex[:8]
    try:
        acts = json.loads(actions) if actions else []
    except Exception:
        acts = []
    cfg.setdefault("automations", []).append({
        "id": aid, "name": name or ("自动化 " + aid),
        "trigger": trigger, "condition": condition,
        "actions": acts, "enabled": True,
        "last_run": 0, "run_count": 0,
    })
    _save(cfg)
    return {"status": "ok", "data": {"id": aid}}


def cmd_automation_toggle(automation_id: str = "", enabled: bool = True) -> dict:
    cfg = _load()
    for a in cfg.get("automations", []):
        if a.get("id") == automation_id:
            a["enabled"] = bool(enabled)
            _save(cfg)
            return {"status": "ok", "data": {"id": automation_id, "enabled": a["enabled"]}}
    return {"status": "error", "msg": "自动化不存在"}


def _run_automations(trigger_type: str, ctx: dict) -> None:
    """内部：触发器命中 → 执行对应动作"""
    cfg = _load()
    changed = False
    for a in cfg.get("automations", []):
        if not a.get("enabled", True):
            continue
        if a.get("trigger") != trigger_type:
            continue
        # 条件过滤（简单子串匹配 ctx 的 JSON）
        cond = a.get("condition") or ""
        if cond:
            cj = json.dumps(ctx, ensure_ascii=False)
            if cond not in cj:
                continue
        for act in a.get("actions", []):
            try:
                t = act.get("type")
                if t == "scene":
                    cmd_scene_apply(act.get("id", ""))
                elif t == "presence":
                    cmd_presence_set(act.get("state", "home"), "automation")
                elif t == "notification":
                    try:
                        from syscall_client import call_syscall
                        call_syscall("notify", {"title": act.get("title", "自动化"),
                                                "body": act.get("body", "")}, timeout=5)
                    except Exception:
                        pass
            except Exception as e:
                logger.debug("automation action failed: %s", e)
        a["last_run"] = int(time.time())
        a["run_count"] = a.get("run_count", 0) + 1
        changed = True
    if changed:
        _save(cfg)


# =============================================================
# 总览
# =============================================================
def cmd_home_overview() -> dict:
    cfg = _load()
    return {"status": "ok", "data": {
        "areas": len(cfg.get("areas", [])),
        "labels": len(cfg.get("labels", [])),
        "entities": len(cfg.get("entities", [])),
        "scenes": len(cfg.get("scenes", [])),
        "zones": len(cfg.get("zones", [])),
        "webhooks": len(cfg.get("webhooks", [])),
        "blueprints": len(cfg.get("blueprints", [])),
        "automations": len(cfg.get("automations", [])),
        "presence": cfg.get("presence", {}).get("state", "unknown"),
        "discovered": len(cfg.get("discovery", {}).get("found", [])),
    }}


def start_background() -> Optional[threading.Thread]:
    """后台线程：定期执行自动化（预留）"""
    return None
