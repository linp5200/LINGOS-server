#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 远程访问（HTTP / WebSocket / VNC）状态与 VNC 控制

- HTTP 8080：WebUI、/api/cmd、文件、健康检查（C http_server）
- WS 2939：App/桌面实时通道（C websocket_server）
- VNC 5900：Wayland 远程桌面（wayvnc，见 distro/desktop/）

命令面（EXT_MAP）：remote_status / vnc_start / vnc_stop
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
from typing import Any, Dict

CONFIG_PATH = "/LINGOS/system/config/remote.json"
DEFAULTS = {"http_port": 8080, "ws_port": 2939, "vnc_port": 5900, "vnc_listen": "127.0.0.1"}


def _cfg() -> Dict[str, Any]:
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict):
            cfg.update(d)
    except Exception:
        pass
    return cfg


def _probe(port: int, host: str = "127.0.0.1", timeout: float = 0.6) -> bool:
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return True
    except Exception:
        return False


def _lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def status() -> Dict[str, Any]:
    cfg = _cfg()
    http_up = _probe(cfg["http_port"])
    ws_up = _probe(cfg["ws_port"])
    vnc_up = _probe(cfg["vnc_port"])
    ip = _lan_ip()
    return {
        "status": "ok",
        "data": {
            "http": {"port": cfg["http_port"], "up": http_up,
                      "url": "http://%s:%s/ui" % (ip, cfg["http_port"])},
            "ws": {"port": cfg["ws_port"], "up": ws_up,
                    "url": "ws://%s:%s" % (ip, cfg["ws_port"])},
            "vnc": {"port": cfg["vnc_port"], "listen": cfg.get("vnc_listen", "127.0.0.1"),
                     "up": vnc_up, "url": "vnc://%s:%s" % (ip, cfg["vnc_port"])},
            "notes": "VNC 默认仅监听 127.0.0.1；对外暴露请加认证或走 SSH 隧道",
        },
    }


def _run(cmd: list) -> Dict[str, Any]:
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=15)
        return {"rc": p.returncode,
                "out": (p.stdout or b"").decode("utf-8", "ignore")[:500],
                "err": (p.stderr or b"").decode("utf-8", "ignore")[:500]}
    except Exception as e:
        return {"rc": -1, "err": str(e)[:300]}


def vnc_start() -> Dict[str, Any]:
    if shutil.which("systemctl"):
        r = _run(["systemctl", "--user", "start", "wayvnc"])
        if r.get("rc") == 0:
            return {"status": "ok", "via": "systemd-user", "detail": r}
    if not shutil.which("wayvnc"):
        return {"status": "error", "msg": "wayvnc 未安装（见 distro/desktop/install.sh）"}
    env = dict(os.environ)
    env.setdefault("XDG_SESSION_TYPE", "wayland")
    try:
        subprocess.Popen([shutil.which("wayvnc") or "wayvnc", "-l",
                          str(_cfg().get("vnc_listen", "127.0.0.1")),
                          str(_cfg()["vnc_port"])], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return {"status": "ok", "via": "spawn"}
    except Exception as e:
        return {"status": "error", "msg": str(e)}


def vnc_stop() -> Dict[str, Any]:
    if shutil.which("systemctl"):
        r = _run(["systemctl", "--user", "stop", "wayvnc"])
        if r.get("rc") == 0:
            return {"status": "ok", "via": "systemd-user", "detail": r}
    r = _run(["pkill", "-f", "wayvnc"])
    return {"status": "ok" if r.get("rc") == 0 else "error", "detail": r}


# ---- 命令面 ----
def cmd_remote_status() -> Dict[str, Any]:
    return status()


def cmd_vnc_start() -> Dict[str, Any]:
    return vnc_start()


def cmd_vnc_stop() -> Dict[str, Any]:
    return vnc_stop()


if __name__ == "__main__":
    print(json.dumps(status(), ensure_ascii=False, indent=2))
