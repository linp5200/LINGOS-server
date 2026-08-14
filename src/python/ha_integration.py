#!/usr/bin/env python3
"""Home Assistant 集成模块（AI-AGENT#10 定稿——2026-08-13 先生裁决）
- B: REST 控制（/api/services——call_service）
- C: WebSocket 实时事件订阅（state_changed → 事件回调）
- A: MQTT 上报由 C 端 mqtt_ha.c 负责（本模块不管）
- 配置：/LINGOS/system/config/ha_config.json（主机本地存凭据）
- 权限：ha_control（默认直接执行；高风险实体——开锁/断电/燃气——强制确认）
"""

import json
import logging
import os
import threading
import time

logger = logging.getLogger("HA")

HA_CONFIG_PATH = "/LINGOS/system/config/ha_config.json"

# 高风险实体关键词（强制确认——先生裁决 A2）
HIGH_RISK_KEYWORDS = ["lock", "unlock", "gas", "valve", "power_off", "power_on",
                      "switch.off", "cover", "alarm_arm", "climate"]

# ============================================================
# 配置读写
# ============================================================

def ha_load_config() -> dict:
    try:
        if os.path.exists(HA_CONFIG_PATH):
            with open(HA_CONFIG_PATH) as f:
                return json.load(f)
    except Exception:
        pass
    return {}

def ha_save_config(cfg: dict) -> None:
    try:
        os.makedirs(os.path.dirname(HA_CONFIG_PATH), exist_ok=True)
        with open(HA_CONFIG_PATH, "w") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception as e:
        logger.error("ha_save_config: %s", e)

# ============================================================
# 高风险判断（强制确认）
# ============================================================

def ha_is_high_risk(domain: str, entity_id: str) -> bool:
    """高风险实体判断：开锁/断电/燃气/窗帘/布防/温控强制确认"""
    text = f"{domain} {entity_id}".lower()
    return any(kw in text for kw in HIGH_RISK_KEYWORDS)

# ============================================================
# B: REST 控制
# ============================================================

def _ha_headers(cfg: dict) -> dict:
    return {"Authorization": "Bearer " + cfg.get("token", ""),
            "Content-Type": "application/json"}

def cmd_ha_config_get() -> dict:
    cfg = ha_load_config()
    return {"status": "ok",
            "data": {"host": cfg.get("host", ""), "port": cfg.get("port", 8123),
                     "configured": bool(cfg.get("host") and cfg.get("token"))}}

def cmd_ha_config_set(host: str, token: str, port: int = 8123) -> dict:
    ha_save_config({"host": host, "token": token, "port": int(port)})
    return {"status": "ok", "msg": "Home Assistant 配置已保存"}

def cmd_ha_status() -> dict:
    """查询 HA 连接状态（REST /api/——版本/名称）"""
    cfg = ha_load_config()
    if not cfg.get("host"):
        return {"status": "error", "code": "not_configured", "msg": "Home Assistant 未配置"}
    try:
        import requests
        r = requests.get(f"http://{cfg['host']}:{cfg.get('port', 8123)}/api/",
                         headers=_ha_headers(cfg), timeout=10)
        if r.status_code == 200:
            return {"status": "ok", "data": r.json()}
        return {"status": "error", "code": "ha_error", "msg": f"HA 返回 {r.status_code}: {r.text[:200]}"}
    except Exception as e:
        return {"status": "error", "code": "network", "msg": f"无法连接 HA: {e}"}

def cmd_ha_control(domain: str, service: str, entity_id: str, data: str = "") -> dict:
    """B REST 控制：调用 HA /api/services/{domain}/{service}
    高风险实体（开锁/断电/燃气等）返回 need_confirm 供 App 弹确认"""
    cfg = ha_load_config()
    if not cfg.get("host"):
        return {"status": "error", "code": "not_configured", "msg": "Home Assistant 未配置"}
    if ha_is_high_risk(domain, entity_id):
        return {"status": "need_confirm",
                "msg": f"高风险操作：{domain}.{service} {entity_id}——请确认后重发（confirm=true）"}
    return _ha_do_control(cfg, domain, service, entity_id, data)

def _ha_do_control(cfg: dict, domain: str, service: str, entity_id: str, data: str) -> dict:
    try:
        import requests
        payload = {"entity_id": entity_id}
        if data:
            try:
                extra = json.loads(data)
                if isinstance(extra, dict):
                    payload.update(extra)
            except Exception:
                pass
        url = f"http://{cfg['host']}:{cfg.get('port', 8123)}/api/services/{domain}/{service}"
        r = requests.post(url, json=payload, headers=_ha_headers(cfg), timeout=10)
        if r.status_code in (200, 201):
            return {"status": "ok", "data": r.json()}
        return {"status": "error", "code": "ha_error", "msg": f"HA 返回 {r.status_code}: {r.text[:200]}"}
    except Exception as e:
        return {"status": "error", "code": "network", "msg": f"控制失败: {e}"}

def cmd_ha_control_confirm(domain: str, service: str, entity_id: str, data: str = "") -> dict:
    """高风险操作确认后执行"""
    cfg = ha_load_config()
    if not cfg.get("host"):
        return {"status": "error", "code": "not_configured", "msg": "Home Assistant 未配置"}
    return _ha_do_control(cfg, domain, service, entity_id, data)

def cmd_ha_states() -> dict:
    """读取全部实体状态（REST /api/states）"""
    cfg = ha_load_config()
    if not cfg.get("host"):
        return {"status": "error", "code": "not_configured", "msg": "Home Assistant 未配置"}
    try:
        import requests
        r = requests.get(f"http://{cfg['host']}:{cfg.get('port', 8123)}/api/states",
                         headers=_ha_headers(cfg), timeout=10)
        if r.status_code == 200:
            return {"status": "ok", "data": r.json()}
        return {"status": "error", "code": "ha_error", "msg": f"HA 返回 {r.status_code}"}
    except Exception as e:
        return {"status": "error", "code": "network", "msg": f"无法连接 HA: {e}"}

# ============================================================
# C: WebSocket 实时事件订阅（state_changed → 事件回调）
# ============================================================

def _ha_ws_loop(on_event) -> None:
    """后台线程：连接 HA WebSocket API，订阅 state_changed。
    事件回调 on_event(dict)——由 ai_server 转发给 App（type=ha_event）"""
    cfg = ha_load_config()
    if not cfg.get("host"):
        return
    try:
        import websocket  # websocket-client
    except ImportError:
        logger.warning("websocket-client 未安装——HA 实时事件不可用（REST 控制仍可用）")
        return
    url = f"ws://{cfg['host']}:{cfg.get('port', 8123)}/api/websocket"
    while True:
        try:
            ws = websocket.create_connection(url, timeout=10)
            # 认证
            msg = json.loads(ws.recv())
            if msg.get("type") != "auth_required":
                ws.close()
                time.sleep(10)
                continue
            ws.send(json.dumps({"type": "auth", "access_token": cfg.get("token", "")}))
            auth = json.loads(ws.recv())
            if auth.get("type") != "auth_ok":
                logger.warning("HA WS 认证失败: %s", auth.get("message", ""))
                ws.close()
                time.sleep(30)
                continue
            # 订阅 state_changed
            ws.send(json.dumps({"id": 1, "type": "subscribe_events",
                                "event_type": "state_changed"}))
            logger.info("HA WebSocket 事件订阅已连接: %s", url)
            while True:
                raw = ws.recv()
                if not raw:
                    break
                evt = json.loads(raw)
                if evt.get("type") == "event":
                    ed = evt.get("event", {})
                    if ed.get("event_type") == "state_changed":
                        data = ed.get("data", {})
                        new_s = data.get("new_state") or {}
                        old_s = data.get("old_state") or {}
                        try:
                            on_event({
                                "type": "ha_event",
                                "entity": data.get("entity_id", ""),
                                "state": new_s.get("state", ""),
                                "old_state": old_s.get("state", "") if old_s else "",
                                "friendly": (new_s.get("attributes") or {}).get("friendly_name", ""),
                                "ts": time.time(),
                            })
                        except Exception:
                            pass
        except Exception as e:
            logger.warning("HA WS 连接中断: %s——30s 后重连", e)
            time.sleep(30)

def ha_start_event_loop(on_event) -> threading.Thread:
    """启动 HA 事件订阅后台线程（daemon——不阻塞 ai_server 退出）"""
    t = threading.Thread(target=_ha_ws_loop, args=(on_event,), daemon=True)
    t.start()
    return t
