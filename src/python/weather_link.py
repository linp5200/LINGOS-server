#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 天气↔预警联动（P3 · 方案2 §4 设计二/三）

功能：
  ① 天气→预警：周期性读取当前天气，超阈值（暴雨概率/高温/低温/大风）→ 生成预警事件
     （广播 App + 通知中心——与 alertd 预警同通道）
  ② 防重复：同类别冷却期内不重复（默认 12h）
  ③ 天气联动自动化（轻量）：高温/低温可触发建议通知（不直接动设备——动设备走规则引擎）

配置：/LINGOS/system/config/weather_link.json
  {"enabled": true, "check_minutes": 30,
   "rain_prob": 80, "temp_high": 38, "temp_low": -8, "wind_ms": 10.8, "cooldown_h": 12}

状态：/LINGOS/state/weather_link_state.json
"""

import json
import logging
import os
import threading
import time

logger = logging.getLogger("WeatherLink")

CONFIG_PATH = "/LINGOS/system/config/weather_link.json"
STATE_PATH = "/LINGOS/state/weather_link_state.json"

_DEFAULTS = {
    "enabled": True,
    "check_minutes": 30,
    "rain_prob": 80,
    "temp_high": 38,
    "temp_low": -8,
    "wind_ms": 10.8,
    "cooldown_h": 12,
}

_started = False


def _load_cfg() -> dict:
    cfg = dict(_DEFAULTS)
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                d = json.load(f) or {}
            if isinstance(d, dict):
                cfg.update(d)
    except Exception as e:
        logger.debug("weather_link config: %s", e)
    return cfg


def _load_state() -> dict:
    try:
        if os.path.exists(STATE_PATH):
            with open(STATE_PATH, encoding="utf-8") as f:
                return json.load(f) or {}
    except Exception:
        pass
    return {}


def _save_state(st: dict) -> None:
    try:
        os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump(st, f, ensure_ascii=False)
    except Exception:
        pass


def _extract_weather() -> dict:
    """读取当前天气摘要（复用 ai_server 缓存逻辑）"""
    try:
        from ai_server import cmd_weather_current
        r = cmd_weather_current()
        if not isinstance(r, dict) or r.get("status") != "ok":
            return {}
        d = r.get("data") or {}
        if not isinstance(d, dict):
            return {}
        out = {
            "temp": d.get("temperature_2m", d.get("temp", d.get("temperature"))),
            "wind": d.get("wind_speed_10m", d.get("wind_speed", d.get("wind"))),
            "code": d.get("weather_code", d.get("code")),
        }
        # 降雨概率：可能来自预报（hourly）——尽力取
        pr = d.get("precipitation_probability", d.get("rain_prob"))
        if pr is None:
            fc = d.get("forecast") or []
            if isinstance(fc, list) and fc:
                f0 = fc[0] if isinstance(fc[0], dict) else {}
                pr = f0.get("precipitation_probability", f0.get("rain_prob"))
        out["rain_prob"] = pr
        return out
    except Exception as e:
        logger.debug("weather extract failed: %s", e)
        return {}


def _num(v):
    try:
        if v is None:
            return None
        return float(v)
    except Exception:
        return None


def _check_once() -> list:
    cfg = _load_cfg()
    if not cfg.get("enabled", True):
        return []
    w = _extract_weather()
    if not w:
        return []

    now = int(time.time())
    st = _load_state()
    cd = max(1, int(cfg.get("cooldown_h", 12))) * 3600
    fired = []

    def _due(kind: str) -> bool:
        return now - int(st.get(kind, 0)) >= cd

    t = _num(w.get("temp"))
    wind = _num(w.get("wind"))
    rain = _num(w.get("rain_prob"))

    if t is not None and t >= float(cfg.get("temp_high", 38)) and _due("high_temp"):
        fired.append({"type": "high_temp", "level": 3,
                      "description": "高温预警：当前 %.1f°C（阈值 %s°C）" % (t, cfg.get("temp_high"))})
        st["high_temp"] = now
    if t is not None and t <= float(cfg.get("temp_low", -8)) and _due("low_temp"):
        fired.append({"type": "low_temp", "level": 3,
                      "description": "低温预警：当前 %.1f°C（阈值 %s°C）" % (t, cfg.get("temp_low"))})
        st["low_temp"] = now
    if rain is not None and rain >= float(cfg.get("rain_prob", 80)) and _due("rain"):
        fired.append({"type": "rain", "level": 2,
                      "description": "暴雨预警：降雨概率 %d%%（阈值 %s%%）" % (rain, cfg.get("rain_prob"))})
        st["rain"] = now
    if wind is not None and wind >= float(cfg.get("wind_ms", 10.8)) and _due("storm"):
        fired.append({"type": "storm", "level": 3,
                      "description": "大风预警：风速 %.1f m/s（阈值 %s m/s）" % (wind, cfg.get("wind_ms"))})
        st["storm"] = now

    if fired:
        _save_state(st)
        for ev in fired:
            ev["source"] = "weather_link"
            try:
                from ai_server import _broadcast_alert_event
                _broadcast_alert_event({"type": "alert_event", "data": ev})
            except Exception as e:
                logger.debug("weather alert broadcast failed: %s", e)
            try:
                from alert_subscription import on_alert_for_notify
                on_alert_for_notify(ev)
            except Exception:
                pass
        logger.warning("weather link fired %d alert(s)", len(fired))
    return fired


def _loop() -> None:
    while True:
        try:
            cfg = _load_cfg()
            _check_once()
            wait_s = max(5, int(cfg.get("check_minutes", 30))) * 60
        except Exception as e:
            logger.warning("weather link loop error: %s", e)
            wait_s = 1800
        time.sleep(wait_s)


def start_weather_link() -> None:
    """启动后台联动线程（幂等）"""
    global _started
    if _started:
        return
    _started = True
    threading.Thread(target=_loop, daemon=True, name="weather_link").start()
    logger.info("weather link started (interval=%s min)", _load_cfg().get("check_minutes", 30))
