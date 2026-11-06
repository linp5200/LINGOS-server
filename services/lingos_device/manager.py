#!/usr/bin/env python3
"""LING OS 0.13 device adapters (standard library + optional integrations).

Protocols:
  virtual  always available (default, used by Web desktop)
  ha       Home Assistant REST (urllib)
  mqtt     MQTT publish (optional paho-mqtt)
  miio     Xiaomi miIO (optional python-miio; experimental)

The manager never raises on adapter errors: it returns {ok:false,error:...} so a
broken LAN device cannot take down the Web desktop.
"""
from __future__ import annotations

import json
import ssl
import urllib.request
from typing import Any


class BaseAdapter:
    protocol = "base"

    def __init__(self, store):
        self.store = store

    def status(self) -> dict:
        return {"protocol": self.protocol, "available": True, "detail": ""}

    def discover(self) -> list[dict]:
        return []

    def command(self, dev: dict, cmd: str, args: dict) -> dict:
        return {"ok": False, "error": "unsupported"}


class VirtualAdapter(BaseAdapter):
    protocol = "virtual"

    def command(self, dev: dict, cmd: str, args: dict) -> dict:
        state = dict(dev.get("state") or {})
        if dev["type"] == "light":
            if cmd in ("on", "off"):
                state["on"] = cmd == "on"
            elif cmd == "toggle":
                state["on"] = not bool(state.get("on"))
            elif cmd == "brightness":
                try:
                    state["brightness"] = max(0, min(100, int(args.get("value", 80))))
                except (TypeError, ValueError):
                    return {"ok": False, "error": "bad brightness"}
        elif dev["type"] == "lock":
            if cmd in ("lock", "unlock"):
                state["locked"] = cmd == "lock"
        elif dev["type"] == "sensor":
            state.update(args or {})
        else:
            state["last_cmd"] = cmd
        return {"ok": True, "state": state}


class HomeAssistantAdapter(BaseAdapter):
    protocol = "ha"

    def _cfg(self):
        return (self.store.setting_get("ha_url", "") or "").rstrip("/"), (self.store.setting_get("ha_token", "") or "")

    def status(self) -> dict:
        url, token = self._cfg()
        return {"protocol": "ha", "available": bool(url and token),
                "detail": url if (url and token) else "配置 ha_url / ha_token"}

    def _req(self, path: str, method: str = "GET", body: dict | None = None):
        base, token = self._cfg()
        if not base or not token:
            raise RuntimeError("Home Assistant 未配置")
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(base + path, data=data, method=method,
                                     headers={"Authorization": "Bearer " + token,
                                              "Content-Type": "application/json"})
        ctx = ssl.create_default_context()
        with urllib.request.urlopen(req, timeout=6, context=ctx) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    def discover(self) -> list[dict]:
        try:
            states = self._req("/api/states")
        except Exception:
            return []
        out = []
        for s in states if isinstance(states, list) else []:
            eid = str(s.get("entity_id", ""))
            if "." not in eid:
                continue
            domain = eid.split(".", 1)[0]
            if domain not in ("light", "switch", "lock", "sensor", "binary_sensor", "cover", "climate"):
                continue
            attrs = s.get("attributes") or {}
            out.append({
                "id": "ha." + eid,
                "name": str(attrs.get("friendly_name") or eid),
                "type": "lock" if domain == "lock" else ("light" if domain == "light" else ("sensor" if "sensor" in domain else "switch")),
                "protocol": "ha",
                "online": str(s.get("state")) not in ("unavailable", "unknown"),
                "state": {"entity_id": eid, "state": s.get("state"), "attributes": attrs},
            })
        return out

    def command(self, dev: dict, cmd: str, args: dict) -> dict:
        eid = str((dev.get("state") or {}).get("entity_id") or "")
        if not eid:
            return {"ok": False, "error": "HA 设备缺少 entity_id"}
        domain = eid.split(".", 1)[0]
        if domain == "lock":
            service = "unlock" if cmd == "unlock" else "lock"
        elif domain == "light":
            service = "turn_on" if cmd in ("on", "toggle") else "turn_off"
        elif domain == "cover":
            service = "open_cover" if cmd == "on" else "close_cover"
        else:
            service = "turn_on" if cmd in ("on", "toggle") else "turn_off"
        try:
            self._req(f"/api/services/{domain}/{service}", "POST", {"entity_id": eid})
            return {"ok": True, "state": {"entity_id": eid, "last_cmd": cmd}}
        except Exception as e:
            return {"ok": False, "error": str(e)}


class MqttAdapter(BaseAdapter):
    protocol = "mqtt"

    def _cfg(self):
        return {
            "host": self.store.setting_get("mqtt_host", "") or "",
            "port": int(self.store.setting_get("mqtt_port", "1883") or 1883),
            "user": self.store.setting_get("mqtt_user", "") or "",
            "password": self.store.setting_get("mqtt_pass", "") or "",
            "prefix": self.store.setting_get("mqtt_prefix", "lingos") or "lingos",
        }

    def status(self) -> dict:
        try:
            import paho.mqtt.client as _m  # noqa: F401
            has = True
        except Exception:
            has = False
        cfg = self._cfg()
        return {"protocol": "mqtt", "available": bool(has and cfg["host"]),
                "detail": ("缺 paho-mqtt" if not has else ("未配置 mqtt_host" if not cfg["host"] else cfg["host"]))}

    def command(self, dev: dict, cmd: str, args: dict) -> dict:
        try:
            import paho.mqtt.publish as publish
        except Exception:
            return {"ok": False, "error": "mqtt 适配器需要 python3-paho-mqtt"}
        cfg = self._cfg()
        if not cfg["host"]:
            return {"ok": False, "error": "未配置 mqtt_host"}
        topic = f"{cfg['prefix']}/{dev['id']}/set"
        payload = json.dumps({"cmd": cmd, "args": args or {}}, ensure_ascii=False)
        auth = {"username": cfg["user"], "password": cfg["password"]} if cfg["user"] else None
        try:
            publish.single(topic, payload, hostname=cfg["host"], port=cfg["port"], auth=auth, qos=0, retain=False)
            return {"ok": True, "state": {"last_cmd": cmd, "topic": topic}}
        except Exception as e:
            return {"ok": False, "error": str(e)}


class MiioAdapter(BaseAdapter):
    protocol = "miio"

    def status(self) -> dict:
        try:
            import miio  # noqa: F401
            has = True
        except Exception:
            has = False
        return {"protocol": "miio", "available": has,
                "detail": "python-miio 已安装" if has else "需要 python-miio 与设备 token（实验）"}

    def discover(self) -> list[dict]:
        raw = self.store.setting_get("miio_devices", "") or ""
        try:
            items = json.loads(raw) if raw else []
        except Exception:
            return []
        out = []
        for it in items if isinstance(items, list) else []:
            out.append({"id": "miio." + str(it.get("did") or it.get("ip")),
                        "name": str(it.get("name") or it.get("ip")),
                        "type": "switch", "protocol": "miio", "online": True,
                        "state": {"ip": it.get("ip"), "did": it.get("did")}})
        return out

    def command(self, dev: dict, cmd: str, args: dict) -> dict:
        try:
            from miio import Device
        except Exception:
            return {"ok": False, "error": "miio 适配器需要 python-miio"}
        st = dev.get("state") or {}
        try:
            d = Device(st.get("ip"), st.get("token", ""))
            if cmd in ("on", "off"):
                getattr(d, cmd)()
            elif cmd == "status":
                return {"ok": True, "state": {"status": str(d.status())}}
            else:
                return {"ok": False, "error": "miio 仅支持 on/off/status（实验）"}
            return {"ok": True, "state": {"last_cmd": cmd}}
        except Exception as e:
            return {"ok": False, "error": str(e)}


class DeviceManager:
    def __init__(self, store):
        self.store = store
        self.adapters = {
            "virtual": VirtualAdapter(store),
            "ha": HomeAssistantAdapter(store),
            "mqtt": MqttAdapter(store),
            "miio": MiioAdapter(store),
        }

    def list(self):
        return self.store.devices()

    def adapter_status(self):
        return [a.status() for a in self.adapters.values()]

    def discover(self, protocol: str = ""):
        out = []
        for name, a in self.adapters.items():
            if protocol and name != protocol:
                continue
            out.extend(a.discover())
        return out

    def add_virtual(self, name: str, dtype: str):
        import secrets
        dtype = dtype if dtype in ("light", "lock", "sensor", "camera", "switch") else "switch"
        did = "virtual." + secrets.token_hex(4)
        state = {"on": False} if dtype in ("light", "switch") else ({"locked": True} if dtype == "lock" else {})
        self.store.add_device(did, name or "虚拟设备", dtype, "virtual", 1, state)
        self.store.event("device", f"add virtual {did} {name}")
        return {"id": did}

    def remove(self, dev_id: str):
        self.store.delete_device(dev_id)
        self.store.event("device", f"remove {dev_id}")

    def command(self, dev_id: str, cmd: str, args: dict):
        dev = self.store.get_device(dev_id)
        if not dev:
            return None
        adapter = self.adapters.get(dev.get("protocol") or "virtual")
        if adapter is None:
            return {"ok": False, "error": "unknown protocol"}
        res = adapter.command(dev, cmd, args or {})
        if res.get("ok"):
            state = res.get("state") or dev.get("state") or {}
            self.store.update_device_state(dev_id, state)
            self.store.event("device", f"{dev_id} {cmd} {json.dumps(args or {}, ensure_ascii=False)}")
        return res
