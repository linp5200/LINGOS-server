#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 生命线投递（§2B B5 · 2026-09-24）

先生设定（框架 §6A 五重保障）：
  ① 多通道并行：WS → ntfy → 声音（本地铃声/TTS）→ HA 声光（任一到达即算送达）
  ② ACK 强制：L3/L4 级需回执——30s 未确认重推；超时升级（通知联系人——可选配置）
  ③ 离线能力：本地传感器 → 本地声光（断网也可响——本机 bell/espeak 不依赖网络）
  ④ 断网兜底：多路径重试 + 恢复后补推（本模块重推循环即兜底）
  ⑤ 延迟指标：各通道时延记录 → /LINGOS/log/crisis_delivery.jsonl

配置：/LINGOS/system/config/crisis_delivery.json（全部可选）:
  {"ntfy_topic":"", "ntfy_server":"https://ntfy.sh",
   "siren_entity":"switch.siren", "bell":true, "tts":true,
   "repush_seconds":30, "contacts":["topic2"], "max_repush":120}
"""

import json
import logging
import os
import subprocess
import threading
import time
import urllib.request

logger = logging.getLogger("CrisisDelivery")

CONFIG_PATH = "/LINGOS/system/config/crisis_delivery.json"
METRICS_PATH = "/LINGOS/log/crisis_delivery.jsonl"

_lock = threading.RLock()
_thread = None
_stop_flag = threading.Event()

_DEFAULTS = {
    "ntfy_topic": "",
    "ntfy_server": "https://ntfy.sh",
    "siren_entity": "",
    "bell": True,
    "tts": True,
    "repush_seconds": 30,
    "contacts": [],
    "max_repush": 120,
}


def _load_cfg() -> dict:
    cfg = dict(_DEFAULTS)
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                user = json.load(f) or {}
            if isinstance(user, dict):
                cfg.update(user)
    except Exception as e:
        logger.debug("crisis_delivery config load failed: %s", e)
    return cfg


def _metric(rec: dict) -> None:
    try:
        rec["ts"] = time.time()
        os.makedirs(os.path.dirname(METRICS_PATH), exist_ok=True)
        with open(METRICS_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception:
        pass


# =============================================================
# 通道：① WS 广播 ② ntfy ③ 声音 ④ HA 声光
# =============================================================

def _send_ws(st: dict, repush: bool = False) -> bool:
    try:
        from crisis import _broadcast_crisis
        _broadcast_crisis(st, "crisis_repush" if repush else "crisis_alert")
        return True
    except Exception as e:
        logger.warning("delivery ws failed: %s", e)
        return False


def _send_ntfy(st: dict, cfg: dict, topic: str = "") -> bool:
    t = topic or cfg.get("ntfy_topic") or ""
    if not t:
        return False
    try:
        server = (cfg.get("ntfy_server") or "https://ntfy.sh").rstrip("/")
        url = f"{server}/{t}"
        title = ("🚨 %s警报" % st.get("name", "危机")).encode("utf-8")
        body = ("%s\n来源: %s\n时间: %s" % (
            st.get("detail", ""), st.get("source", ""),
            time.strftime("%H:%M:%S", time.localtime(st.get("started_at", time.time())))
        )).encode("utf-8")
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Title", title.decode("utf-8"))
        req.add_header("Priority", "urgent")
        req.add_header("Tags", "rotating_light")
        with urllib.request.urlopen(req, timeout=8) as r:
            return 200 <= r.status < 300
    except Exception as e:
        logger.warning("delivery ntfy failed: %s", e)
        return False


def _send_sound(st: dict, cfg: dict) -> bool:
    """离线能力：本机声音（不依赖网络）——终端 bell + TTS（espeak-ng 若可用）"""
    ok = False
    if cfg.get("bell", True):
        try:
            # 写 stderr 的 bell——多进程日志不污染（不落文件）
            import sys
            for _ in range(5):
                sys.stderr.write("\a")
            sys.stderr.flush()
            ok = True
        except Exception:
            pass
    if cfg.get("tts", True):
        try:
            engine = None
            for cand in ("espeak-ng", "espeak"):
                from shutil import which
                if which(cand):
                    engine = cand
                    break
            if engine:
                text = "%s警报。%s" % (st.get("name", "危机"), st.get("detail", "")[:80])
                subprocess.Popen([engine, "-v", "zh", "-s", "150", text],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ok = True
        except Exception:
            pass
    return ok


def _send_ha_siren(st: dict, cfg: dict) -> bool:
    ent = cfg.get("siren_entity") or ""
    if not ent:
        return False
    try:
        from ha_integration import ha_load_config, _ha_do_control
        hcfg = ha_load_config()
        if not hcfg or not hcfg.get("host"):
            return False
        domain = ent.split(".", 1)[0]
        r = _ha_do_control(hcfg, domain, "turn_on", ent, "")
        return bool(isinstance(r, dict) and r.get("status") == "ok")
    except Exception as e:
        logger.debug("delivery ha siren failed: %s", e)
        return False


def _deliver_round(st: dict, cfg: dict, repush: bool) -> list:
    """一轮多通道并行投递——返回各通道结果（含时延）"""
    results = []
    channels = [
        ("ws",    lambda: _send_ws(st, repush)),
        ("ntfy",  lambda: _send_ntfy(st, cfg)),
        ("sound", lambda: _send_sound(st, cfg)),
        ("ha_siren", lambda: _send_ha_siren(st, cfg)),
    ]
    threads = []
    out = {}

    def _run(name, fn):
        t0 = time.time()
        try:
            okc = bool(fn())
        except Exception:
            okc = False
        out[name] = {"ok": okc, "latency_ms": int((time.time() - t0) * 1000)}

    for name, fn in channels:
        th = threading.Thread(target=_run, args=(name, fn), daemon=True)
        th.start()
        threads.append(th)
    for th in threads:
        th.join(timeout=10)

    for name, _ in channels:
        r = out.get(name, {"ok": False, "latency_ms": -1})
        results.append({"channel": name, **r})
    return results


# =============================================================
# 投递循环（ACK 强制 + 30s 重推 + 断网兜底 + 指标）
# =============================================================

def _delivery_loop(crisis_id: str) -> None:
    cfg = _load_cfg()
    repush_s = int(cfg.get("repush_seconds", 30) or 30)
    max_repush = int(cfg.get("max_repush", 120) or 120)
    n = 0
    t0 = time.time()

    while not _stop_flag.is_set():
        try:
            from crisis import crisis_get
            st = crisis_get()
        except Exception:
            break
        if not st.get("active") or st.get("crisis_id") != crisis_id:
            _metric({"event": "end", "crisis_id": crisis_id, "reason": "resolved_or_changed"})
            break

        results = _deliver_round(st, cfg, repush=(n > 0))
        _metric({"event": "round", "crisis_id": crisis_id, "round": n,
                 "elapsed_ms": int((time.time() - t0) * 1000), "results": results})

        if st.get("acked"):
            _metric({"event": "acked", "crisis_id": crisis_id,
                     "ack_latency_ms": int((time.time() - t0) * 1000)})
            logger.info("crisis %s ACK received — delivery loop ends", crisis_id)
            break

        # 5 分钟未确认 → 升级通知联系人（可选配置）
        if n == 10 and cfg.get("contacts"):
            for c in cfg.get("contacts", []):
                try:
                    _send_ntfy(st, cfg, topic=str(c))
                except Exception:
                    pass
            _metric({"event": "escalated", "crisis_id": crisis_id})

        n += 1
        if n > max_repush:
            _metric({"event": "end", "crisis_id": crisis_id, "reason": "max_repush"})
            break

        # 等待（可被 stop 打断）
        _stop_flag.wait(timeout=repush_s)


def start_delivery(st: dict) -> None:
    """危机触发 → 启动投递（幂等：先停旧循环）"""
    global _thread
    with _lock:
        stop_delivery()
        _stop_flag.clear()
        cid = st.get("crisis_id", "")
        _thread = threading.Thread(target=_delivery_loop, args=(cid,), daemon=True)
        _thread.start()
        _metric({"event": "start", "crisis_id": cid, "type": st.get("crisis_type", "")})


def stop_delivery() -> None:
    _stop_flag.set()
    global _thread
    _thread = None


# =============================================================
# ACK 查询（供状态展示/调试）
# =============================================================
def delivery_status() -> dict:
    try:
        from crisis import crisis_get
        st = crisis_get()
        return {"status": "ok", "data": {
            "active": bool(st.get("active")),
            "acked": bool(st.get("acked")),
            "acked_at": st.get("acked_at", 0),
            "running": bool(_thread and _thread.is_alive())}}
    except Exception as e:
        return {"status": "error", "msg": str(e)}
