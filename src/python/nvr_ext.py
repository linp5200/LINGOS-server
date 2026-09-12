#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 监控/NVR 扩展（先生 2026-09-12 裁决 · 批次3）

对标 Frigate 核心能力，补齐 7 项缺失：
  M9  移动侦测前置    —— 低开销帧差，有运动才做 AI 检测（省 90% 算力）
  M10 时间线回放      —— 录像索引 + 时间轴查询
  M11 对象录像保留    —— 有对象的录像延长保留
  M12 存储管理/循环覆盖 —— 空间监控 + 最旧删除
  M13 ONVIF           —— 设备发现 + 能力探测
  M14 RTSP 重流       —— 单连接多消费（减少摄像头连接数）
  M15 动态合成视图    —— 多路合成单路

设计：本模块**不依赖 ffmpeg**（仅做索引/管理/协议），
      实际拉流由 monitor_service / rtsp_streamer 负责。
"""

import json
import os
import time
import logging
import threading
import shutil
import struct
import socket
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger("NvrExt")

CONFIG_PATH = "/LINGOS/system/config/nvr_ext.json"
RECORD_DIR = "/LINGOS/data/recordings"
INDEX_PATH = "/LINGOS/data/recordings/index.json"

_lock = threading.RLock()
_motion_state: Dict[str, dict] = {}      # camera_id -> 移动侦测状态
_restream_registry: Dict[str, dict] = {}  # camera_id -> 重流信息


# =============================================================
# 配置
# =============================================================
def _default_config() -> dict:
    return {
        "version": 1,
        "cameras": {},          # cam_id -> {name, rtsp, retention_days, ...}
        "retention": {
            "default_days": 7,      # 普通录像保留
            "object_days": 30,      # 有对象的录像保留（M11）
            "max_storage_gb": 50,   # 存储上限（M12）
            "reserve_gb": 5,        # 保留空间
        },
        "motion": {                 # M9
            "enabled": True,
            "threshold": 0.02,      # 帧差比例阈值
            "cooldown_s": 5,        # 触发后冷却
            "pre_buffer": 3,        # 前置缓冲秒数
        },
        "onvif": {"discovered": [], "last_scan": 0},   # M13
        "restream": {"enabled": True, "port_base": 8900},  # M14
        "composite": {"views": []},  # M15
        "index": [],                # M10 录像索引
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
            logger.warning("load nvr config failed: %s", e)
        return _default_config()


def _save(cfg: dict) -> None:
    with _lock:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        tmp = CONFIG_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        os.replace(tmp, CONFIG_PATH)


# =============================================================
# M9 · 移动侦测前置（低开销帧差）
# =============================================================
def motion_check(camera_id: str, frame_bytes: bytes,
                 width: int = 0, height: int = 0,
                 threshold: float = None) -> dict:
    """对一帧做**轻量**运动检测（灰度降采样 + 帧差）

    返回 {"motion": bool, "ratio": float, "action": "detect"|"skip"}
    AI 检测仅在 motion=True 时进行 —— 这是 Frigate 的核心优化。
    """
    cfg = _load()
    mcfg = cfg.get("motion", {})
    thr = threshold if threshold is not None else mcfg.get("threshold", 0.02)
    cd = mcfg.get("cooldown_s", 5)

    st = _motion_state.setdefault(camera_id, {"prev": None, "last_trig": 0, "checks": 0, "hits": 0})
    st["checks"] += 1

    if not mcfg.get("enabled", True):
        return {"motion": True, "ratio": 1.0, "action": "detect",
                "note": "移动侦测已关闭（全部帧送 AI）"}

    # 降采样：每 64 字节取 1（快速且足够判断运动）
    if not frame_bytes:
        return {"motion": False, "ratio": 0.0, "action": "skip"}

    step = max(1, len(frame_bytes) // 2048)
    sample = frame_bytes[::step][:2048]
    cur = bytes((b >> 4) for b in sample)     # 粗略灰度

    prev = st.get("prev")
    st["prev"] = cur

    if prev is None or len(prev) != len(cur):
        return {"motion": True, "ratio": 1.0, "action": "detect", "note": "首帧"}

    diff = sum(1 for a, b in zip(cur, prev) if abs(a - b) > 3)
    ratio = diff / max(1, len(cur))

    now = time.time()
    cooling = (now - st.get("last_trig", 0)) < cd

    if ratio >= thr and not cooling:
        st["last_trig"] = now
        st["hits"] += 1
        try:
            _index_add_event(camera_id, "motion", {"ratio": round(ratio, 4)})
        except Exception:
            pass
        logger.debug("motion detected cam=%s ratio=%.3f", camera_id, ratio)
        return {"motion": True, "ratio": round(ratio, 4), "action": "detect",
                "pre_buffer_s": mcfg.get("pre_buffer", 3)}

    return {"motion": False, "ratio": round(ratio, 4), "action": "skip",
            "cooling": cooling}


def cmd_motion_config(threshold: float = None, cooldown_s: int = None,
                      enabled: bool = None, pre_buffer: int = None) -> dict:
    cfg = _load()
    m = cfg.setdefault("motion", {})
    if threshold is not None:
        m["threshold"] = max(0.001, min(0.5, float(threshold)))
    if cooldown_s is not None:
        m["cooldown_s"] = max(0, int(cooldown_s))
    if enabled is not None:
        m["enabled"] = bool(enabled)
    if pre_buffer is not None:
        m["pre_buffer"] = max(0, min(30, int(pre_buffer)))
    _save(cfg)
    return {"status": "ok", "data": m}


def cmd_motion_stats() -> dict:
    return {"status": "ok", "data": {
        cam: {"checks": s.get("checks", 0), "hits": s.get("hits", 0),
              "last": s.get("last_trig", 0)}
        for cam, s in _motion_state.items()
    }}


# =============================================================
# M10 · 时间线回放（录像索引）
# =============================================================
def _index_add_event(camera_id: str, kind: str, detail: dict = None) -> None:
    cfg = _load()
    idx = cfg.setdefault("index", [])
    idx.append({
        "id": "%d-%s" % (int(time.time() * 1000), camera_id),
        "camera": camera_id, "kind": kind, "detail": detail or {},
        "ts": int(time.time()),
    })
    if len(idx) > 5000:
        del idx[:len(idx) - 5000]
    _save(cfg)


def cmd_timeline_query(camera: str = "", start: int = 0, end: int = 0,
                       kind: str = "", limit: int = 200) -> dict:
    cfg = _load()
    now = int(time.time())
    s = start or (now - 86400)
    e = end or now
    items = [x for x in cfg.get("index", [])
             if s <= x.get("ts", 0) <= e
             and (not camera or x.get("camera") == camera)
             and (not kind or x.get("kind") == kind)]
    items.sort(key=lambda x: x.get("ts", 0))
    return {"status": "ok", "data": {
        "start": s, "end": e, "count": len(items),
        "items": items[-max(1, min(limit, 1000)):],
    }}


def cmd_timeline_segments(camera: str = "", hours: int = 24) -> dict:
    """按小时聚合，生成时间轴密度条（供 UI 绘制）"""
    cfg = _load()
    now = int(time.time())
    since = now - max(1, hours) * 3600
    buckets: Dict[int, dict] = {}
    for x in cfg.get("index", []):
        ts = x.get("ts", 0)
        if ts < since or (camera and x.get("camera") != camera):
            continue
        hb = ts - (ts % 3600)
        b = buckets.setdefault(hb, {"hour": hb, "motion": 0, "object": 0, "other": 0})
        k = x.get("kind", "other")
        b[k if k in ("motion", "object") else "other"] += 1
    segs = [buckets[k] for k in sorted(buckets.keys())]
    return {"status": "ok", "data": {"hours": hours, "segments": segs,
                                     "total": sum(s["motion"] + s["object"] + s["other"] for s in segs)}}


def cmd_record_index_add(camera: str = "", path: str = "", start: int = 0,
                         end: int = 0, has_object: bool = False,
                         objects: str = "") -> dict:
    """登记一段录像（供回放/保留策略使用）"""
    cfg = _load()
    try:
        objs = json.loads(objects) if objects else []
    except Exception:
        objs = []
    rec = {
        "id": "%d-%s" % (int(time.time() * 1000), camera or "cam"),
        "camera": camera, "path": path,
        "start": start or int(time.time()), "end": end or int(time.time()),
        "has_object": bool(has_object), "objects": objs,
        "size": os.path.getsize(path) if path and os.path.exists(path) else 0,
    }
    cfg.setdefault("recordings", []).append(rec)
    _save(cfg)
    return {"status": "ok", "data": rec}


# =============================================================
# M11 · 对象录像保留
# =============================================================
def cmd_record_mark_object(camera: str = "", path: str = "",
                           objects: str = "") -> dict:
    """把某段录像标记为「含对象」→ 享受更长保留期"""
    cfg = _load()
    try:
        objs = json.loads(objects) if objects else []
    except Exception:
        objs = []
    hit = None
    for r in cfg.get("recordings", []):
        if r.get("path") == path or (camera and r.get("camera") == camera and
                                     abs(r.get("start", 0) - int(time.time())) < 60):
            r["has_object"] = True
            r["objects"] = objs
            hit = r
            break
    if not hit:
        # 未找到则新建
        return cmd_record_index_add(camera, path, 0, 0, True, objects)
    _save(cfg)
    return {"status": "ok", "data": hit}


def _retention_days_for(rec: dict, cfg: dict) -> int:
    r = cfg.get("retention", {})
    return int(r.get("object_days", 30) if rec.get("has_object")
               else r.get("default_days", 7))


def cmd_retention_config(default_days: int = None, object_days: int = None,
                         max_storage_gb: float = None, reserve_gb: float = None) -> dict:
    cfg = _load()
    r = cfg.setdefault("retention", {})
    if default_days is not None:
        r["default_days"] = max(1, int(default_days))
    if object_days is not None:
        r["object_days"] = max(1, int(object_days))
    if max_storage_gb is not None:
        r["max_storage_gb"] = max(1, float(max_storage_gb))
    if reserve_gb is not None:
        r["reserve_gb"] = max(0, float(reserve_gb))
    _save(cfg)
    return {"status": "ok", "data": r}


# =============================================================
# M12 · 存储管理 / 循环覆盖
# =============================================================
def _dir_size_gb(path: str) -> float:
    total = 0
    try:
        for base, _, files in os.walk(path):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(base, f))
                except Exception:
                    pass
    except Exception:
        pass
    return total / (1024 ** 3)


def cmd_storage_status() -> dict:
    cfg = _load()
    r = cfg.get("retention", {})
    used = _dir_size_gb(RECORD_DIR)
    limit = float(r.get("max_storage_gb", 50))
    free = 0.0
    try:
        st = os.statvfs("/LINGOS")
        free = (st.f_bavail * st.f_frsize) / (1024 ** 3)
    except Exception:
        pass
    recs = cfg.get("recordings", [])
    return {"status": "ok", "data": {
        "recordings_dir": RECORD_DIR,
        "used_gb": round(used, 2),
        "limit_gb": limit,
        "usage_pct": round(used / limit * 100, 1) if limit else 0,
        "disk_free_gb": round(free, 2),
        "recordings": len(recs),
        "object_recordings": sum(1 for x in recs if x.get("has_object")),
    }}


def cmd_storage_cleanup(force: bool = False) -> dict:
    """按保留策略清理：① 过期删除 ② 超限时删最旧（循环覆盖）"""
    cfg = _load()
    r = cfg.get("retention", {})
    now = int(time.time())
    recs = cfg.get("recordings", [])
    removed, freed = [], 0.0

    # ① 过期
    keep = []
    for rec in recs:
        days = _retention_days_for(rec, cfg)
        start = rec.get("start", 0)
        if start and (now - start) > days * 86400:
            p = rec.get("path")
            if p and os.path.exists(p):
                try:
                    freed += os.path.getsize(p) / (1024 ** 3)
                    os.remove(p)
                except Exception as e:
                    logger.debug("remove failed: %s", e)
            removed.append({"path": p, "reason": "expired"})
        else:
            keep.append(rec)

    # ② 超限 → 删最旧（优先删无对象的）
    used = _dir_size_gb(RECORD_DIR)
    limit = float(r.get("max_storage_gb", 50)) - float(r.get("reserve_gb", 5))
    if (used > limit or force) and keep:
        keep.sort(key=lambda x: (x.get("has_object", False), x.get("start", 0)))
        while keep and _dir_size_gb(RECORD_DIR) > limit:
            rec = keep.pop(0)
            p = rec.get("path")
            if p and os.path.exists(p):
                try:
                    freed += os.path.getsize(p) / (1024 ** 3)
                    os.remove(p)
                except Exception:
                    pass
            removed.append({"path": p, "reason": "over_limit"})
            if len(removed) > 500:
                break

    cfg["recordings"] = keep
    _save(cfg)
    logger.info("storage cleanup: removed=%d freed=%.2fGB", len(removed), freed)
    return {"status": "ok", "data": {"removed": len(removed), "freed_gb": round(freed, 2),
                                     "details": removed[:50]}}


def cmd_storage_policy() -> dict:
    cfg = _load()
    return {"status": "ok", "data": cfg.get("retention", {})}


# =============================================================
# M13 · ONVIF（设备发现 + 能力探测）
# =============================================================
ONVIF_MULTICAST = ("239.255.255.250", 3702)


def _onvif_ws_discovery(timeout: float = 3.0) -> List[dict]:
    """WS-Discovery：向 239.255.255.250:3702 发送 Probe，收集摄像头应答"""
    msg_id = "uuid:" + os.urandom(8).hex() + "-0000-1000-8000-00805f9b34fb"
    body = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"'
        ' xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"'
        ' xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"'
        ' xmlns:dn="http://www.onvif.org/ver10/network/wsdl">'
        '<e:Header><w:MessageID>' + msg_id + '</w:MessageID>'
        '<w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>'
        '<w:Action e:mustUnderstand="true">'
        'http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>'
        '<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>'
        '</e:Envelope>'
    ).encode()

    found = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.settimeout(timeout)
        s.sendto(body, ONVIF_MULTICAST)
        deadline = time.time() + timeout
        seen = set()
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(8192)
            except socket.timeout:
                break
            except Exception:
                break
            text = data.decode("utf-8", "ignore")
            xaddr = ""
            i = text.find("<w:XAddrs>")
            if i < 0:
                i = text.find("XAddrs>")
            if i >= 0:
                j = text.find("</", i)
                seg = text[i:j] if j > i else ""
                if ">" in seg:
                    xaddr = seg.split(">", 1)[1].strip()
            info = {"host": addr[0], "xaddr": xaddr}
            if addr[0] not in seen:
                seen.add(addr[0])
                found.append(info)
        s.close()
    except Exception as e:
        logger.debug("onvif discovery failed: %s", e)
    return found


def cmd_onvif_scan(timeout: float = 3.0) -> dict:
    found = _onvif_ws_discovery(timeout)
    cfg = _load()
    known = {x.get("host"): x for x in cfg.get("onvif", {}).get("discovered", [])}
    for f in found:
        known[f["host"]] = f
    cfg.setdefault("onvif", {})["discovered"] = list(known.values())
    cfg["onvif"]["last_scan"] = int(time.time())
    _save(cfg)
    return {"status": "ok", "data": {"found": found, "total": len(known),
                                     "scanned_at": cfg["onvif"]["last_scan"]}}


def cmd_onvif_list() -> dict:
    cfg = _load()
    o = cfg.get("onvif", {})
    return {"status": "ok", "data": {"devices": o.get("discovered", []),
                                     "last_scan": o.get("last_scan", 0)}}


def cmd_onvif_probe(host: str = "", port: int = 80) -> dict:
    """探测 ONVIF 设备能力（GetCapabilities 简化版：仅做端口/服务探测）"""
    if not host:
        return {"status": "error", "msg": "缺少 host"}
    result = {"host": host, "port": port, "reachable": False, "services": []}
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        if s.connect_ex((host, port)) == 0:
            result["reachable"] = True
            for p, name in ((80, "HTTP"), (554, "RTSP"), (8000, "ONVIF-HTTP"),
                            (8080, "HTTP-alt"), (2020, "ONVIF")):
                s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s2.settimeout(1.0)
                if s2.connect_ex((host, p)) == 0:
                    result["services"].append({"port": p, "name": name})
                s2.close()
        s.close()
    except Exception as e:
        result["error"] = str(e)
    return {"status": "ok", "data": result}


# =============================================================
# M14 · RTSP 重流（单连接多消费）
# =============================================================
def cmd_restream_add(camera_id: str = "", source: str = "",
                     port: int = 0) -> dict:
    """登记一路重流：源 RTSP → 本地单连接 → 多消费者共享"""
    if not camera_id or not source:
        return {"status": "error", "msg": "缺少 camera_id 或 source"}
    cfg = _load()
    rcfg = cfg.setdefault("restream", {"enabled": True, "port_base": 8900})
    used = {v.get("port") for v in _restream_registry.values()}
    p = port or (rcfg.get("port_base", 8900) + len(_restream_registry))
    while p in used:
        p += 1
    _restream_registry[camera_id] = {
        "camera": camera_id, "source": source, "port": p,
        "consumers": 0, "created": int(time.time()),
    }
    cfg.setdefault("cameras", {}).setdefault(camera_id, {})["restream_port"] = p
    _save(cfg)
    logger.info("restream registered: %s → :%d", camera_id, p)
    return {"status": "ok", "data": _restream_registry[camera_id]}


def cmd_restream_list() -> dict:
    return {"status": "ok", "data": list(_restream_registry.values())}


def cmd_restream_remove(camera_id: str = "") -> dict:
    r = _restream_registry.pop(camera_id, None)
    return {"status": "ok", "data": {"removed": bool(r)}}


def cmd_restream_attach(camera_id: str = "") -> dict:
    """增加一个消费者（共享同一上游连接）"""
    r = _restream_registry.get(camera_id)
    if not r:
        return {"status": "error", "msg": "重流不存在"}
    r["consumers"] = r.get("consumers", 0) + 1
    return {"status": "ok", "data": r}


def cmd_restream_detach(camera_id: str = "") -> dict:
    r = _restream_registry.get(camera_id)
    if not r:
        return {"status": "error", "msg": "重流不存在"}
    r["consumers"] = max(0, r.get("consumers", 0) - 1)
    return {"status": "ok", "data": r}


# =============================================================
# M15 · 动态合成视图（多路合成）
# =============================================================
def cmd_composite_list() -> dict:
    return {"status": "ok", "data": _load().get("composite", {}).get("views", [])}


def cmd_composite_add(name: str = "", cameras: str = "",
                      layout: str = "grid") -> dict:
    """cameras: 逗号分隔 id 列表；layout: grid/2x2/1+3/auto"""
    if not cameras:
        return {"status": "error", "msg": "缺少 cameras"}
    cfg = _load()
    cams = [x.strip() for x in cameras.split(",") if x.strip()]
    vid = uuid_id = os.urandom(4).hex()
    n = len(cams)
    cols = 1 if n <= 1 else (2 if n <= 4 else (3 if n <= 9 else 4))
    rows = (n + cols - 1) // cols
    view = {
        "id": vid, "name": name or ("合成视图 " + vid),
        "cameras": cams, "layout": layout or "grid",
        "cols": cols, "rows": rows,
        "cell_w": 1920 // cols, "cell_h": 1080 // rows,
        "created": int(time.time()),
    }
    cfg.setdefault("composite", {}).setdefault("views", []).append(view)
    _save(cfg)
    return {"status": "ok", "data": view}


def cmd_composite_remove(view_id: str = "") -> dict:
    cfg = _load()
    views = cfg.get("composite", {}).get("views", [])
    before = len(views)
    cfg["composite"]["views"] = [v for v in views if v.get("id") != view_id]
    _save(cfg)
    return {"status": "ok", "data": {"removed": before - len(cfg["composite"]["views"])}}


def cmd_composite_render_plan(view_id: str = "") -> dict:
    """生成合成渲染计划（坐标布局，供渲染端执行）"""
    cfg = _load()
    v = next((x for x in cfg.get("composite", {}).get("views", [])
              if x.get("id") == view_id), None)
    if not v:
        return {"status": "error", "msg": "视图不存在"}
    plan = []
    w, h = v.get("cell_w", 960), v.get("cell_h", 540)
    for i, cam in enumerate(v.get("cameras", [])):
        col = i % max(1, v.get("cols", 1))
        row = i // max(1, v.get("cols", 1))
        plan.append({"camera": cam, "x": col * w, "y": row * h, "w": w, "h": h})
    return {"status": "ok", "data": {"view": v.get("name"), "cells": plan,
                                     "canvas": {"w": w * v.get("cols", 1),
                                                "h": h * v.get("rows", 1)}}}


# =============================================================
# 总览
# =============================================================
def cmd_nvr_overview() -> dict:
    cfg = _load()
    st = cmd_storage_status()["data"]
    return {"status": "ok", "data": {
        "cameras": len(cfg.get("cameras", {})),
        "recordings": st["recordings"],
        "object_recordings": st["object_recordings"],
        "storage_used_gb": st["used_gb"],
        "storage_limit_gb": st["limit_gb"],
        "motion": cfg.get("motion", {}).get("enabled", True),
        "onvif_devices": len(cfg.get("onvif", {}).get("discovered", [])),
        "restreams": len(_restream_registry),
        "composite_views": len(cfg.get("composite", {}).get("views", [])),
        "timeline_events": len(cfg.get("index", [])),
    }}
