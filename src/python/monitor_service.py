#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS 监控系统服务（monitor_service.py —— 先生 2026-09-05 类目架构）
职责（monitor.*）：摄像头/视频采集、多路管理、预览(MJPEG)、快照、录像——【给人看】同正常监控
与 AI 识别引擎(ai_vision)分离：本服务只采集/存储/预览，不做检测识别；
识别引擎消费本服务帧做检测，结果经"叠加开关"才显示到监控画面。

用法:
  monitor_service.py --config /LINGOS/system/config/monitor.json   (常驻)
  monitor_service.py --scan                                        (扫描设备后退出)
"""
import json, os, sys, time, threading, subprocess, socket, argparse, logging
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = logging.getLogger("MonitorService")
MONITOR_CFG = "/LINGOS/system/config/monitor.json"
DEFAULT_CFG = {
    "cameras": [
        # {"id":"cam0","type":"v4l2","device":"/dev/video0","width":640,"height":480,"fps":10,"enabled":true}
        # {"id":"cam1","type":"rtsp","url":"rtsp://...","frame_port":8890,"http_port":8891,"enabled":false}
    ],
    "snapshot_dir": "/LINGOS/data/monitor/snapshots",
    "record_dir": "/LINGOS/data/monitor/recordings",
    "record_segment_min": 10,
    "free_space_reserve_mb": 500,
    "preview": {"port": 8891, "quality": 85},
}
_state = {"running": True, "cameras": [], "frames": {}, "start": time.time()}
_cfg = dict(DEFAULT_CFG)
_lock = threading.Lock()

def log_init():
    logging.basicConfig(level=logging.INFO,
                        format="[%(asctime)s][%(levelname)s][MonitorService] %(message)s")

def load_cfg(path=MONITOR_CFG):
    global _cfg
    try:
        with open(path, encoding="utf-8") as f:
            c = json.load(f)
        if isinstance(c, dict):
            for k in DEFAULT_CFG:
                if k not in c:
                    c[k] = DEFAULT_CFG[k]
            _cfg = c
        LOG.info("config loaded: %s", path)
    except Exception as e:
        LOG.warning("config load fail(%s) use default: %s", e, path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(_cfg, f, ensure_ascii=False, indent=2)

def save_cfg():
    os.makedirs(os.path.dirname(MONITOR_CFG), exist_ok=True)
    with open(MONITOR_CFG, "w", encoding="utf-8") as f:
        json.dump(_cfg, f, ensure_ascii=False, indent=2)

def scan_v4l2():
    """扫描 V4L2 设备 /dev/video*"""
    out = []
    for i in range(8):
        if os.path.exists("/dev/video%d" % i):
            out.append({"id": "cam%d" % i, "type": "v4l2", "device": "/dev/video%d" % i})
    return out

def camera_status(cam):
    """单路状态（不真开流——探测可达性/存在性）"""
    st = {"id": cam.get("id"), "type": cam.get("type"),
          "enabled": cam.get("enabled", True), "connected": False,
          "preview_url": "", "error": ""}
    if cam.get("type") == "v4l2":
        dev = cam.get("device", "/dev/video0")
        st["connected"] = os.path.exists(dev)
        st["error"] = "" if st["connected"] else "device missing: %s" % dev
    elif cam.get("type") == "rtsp":
        hp = cam.get("http_port", 8891)
        try:
            s = socket.create_connection(("127.0.0.1", int(hp)), timeout=1)
            s.close(); st["connected"] = True
            st["preview_url"] = "http://127.0.0.1:%d/" % hp
        except Exception:
            st["connected"] = False
            st["error"] = "rtsp streamer not reachable on %d" % hp
    return st

def cmd_list():
    """monitor_list：列出全部摄像头及状态"""
    with _lock:
        cams = _cfg.get("cameras", [])
    items = [camera_status(c) for c in cams]
    # 若未配置任何摄像头——自动探测 v4l2 设备提示
    if not cams:
        v4 = scan_v4l2()
        items = [camera_status({**c, "enabled": False}) for c in v4]
    return {"status": "ok", "data": {"cameras": items, "count": len(items)}}

def cmd_status():
    """monitor_status：监控系统整体状态"""
    l = cmd_list()
    return {"status": "ok", "data": {
        "service": "monitor", "running": _state["running"],
        "uptime": int(time.time() - _state["start"]),
        "cameras": l["data"]["cameras"], "count": l["data"]["count"],
        "snapshot_dir": _cfg.get("snapshot_dir"), "record_dir": _cfg.get("record_dir"),
        "preview_port": _cfg.get("preview", {}).get("port", 8891),
    }}

def cmd_config_get():
    return {"status": "ok", "data": _cfg}

def cmd_config_set(key, value):
    """白名单简单键设置（嵌套用点：cameras.0.url——本版支持顶层）"""
    top = {"snapshot_dir": str, "record_dir": str, "record_segment_min": int,
           "free_space_reserve_mb": int}
    if key not in top:
        return {"status": "error", "msg": "unknown key: %s" % key}
    try:
        _cfg[key] = top[key](value)
    except Exception as e:
        return {"status": "error", "msg": str(e)}
    save_cfg()
    return {"status": "ok", "key": key, "value": _cfg[key]}

def cmd_snapshot(camera_id="cam0"):
    """monitor_snapshot：抓拍（V4L2 用 ffmpeg 单帧；RTSP 从 MJPEG 流取首帧）返回路径/说明
    （纯 JPEG base64 由 ai_server cmd_camera_snapshot 兼容提供——本服务落盘文件）"""
    import base64
    cams = _cfg.get("cameras", [])
    cam = None
    for c in cams:
        if c.get("id") == camera_id:
            cam = c; break
    if not cam:
        # 自动探测
        v4 = scan_v4l2()
        if v4:
            cam = {**v4[0], "enabled": False}
        else:
            return {"status": "error", "msg": "camera %s not found & no v4l2 device" % camera_id}
    os.makedirs(_cfg.get("snapshot_dir", "/LINGOS/data/monitor/snapshots"), exist_ok=True)
    fn = "%s/%s_%d.jpg" % (_cfg.get("snapshot_dir"), camera_id, int(time.time()))
    if cam.get("type") == "rtsp" and cam.get("url"):
        # ffmpeg 单帧
        try:
            r = subprocess.run(["ffmpeg", "-y", "-i", cam["url"], "-frames:v", "1",
                                "-q:v", "3", fn], capture_output=True, timeout=15)
        except FileNotFoundError:
            return {"status": "error", "msg": "ffmpeg 未安装——快照需 ffmpeg"}
        if r.returncode == 0 and os.path.exists(fn):
            return {"status": "ok", "data": {"file": fn,
                    "b64": base64.b64encode(open(fn, "rb").read()).decode() if os.path.getsize(fn) < 2_000_000 else ""}}
        return {"status": "error", "msg": "ffmpeg capture failed: %s" % cam["url"]}
    elif cam.get("type") == "v4l2":
        dev = cam.get("device", "/dev/video0")
        if not os.path.exists(dev):
            return {"status": "error", "msg": "no v4l2 device: %s（monitor_list 查看可用摄像头）" % dev}
        try:
            r = subprocess.run(["ffmpeg", "-y", "-f", "v4l2", "-i", dev, "-frames:v", "1",
                                "-q:v", "3", fn], capture_output=True, timeout=15)
        except FileNotFoundError:
            return {"status": "error", "msg": "ffmpeg 未安装——快照需 ffmpeg（apt install ffmpeg）"}
        if r.returncode == 0 and os.path.exists(fn):
            return {"status": "ok", "data": {"file": fn,
                    "b64": base64.b64encode(open(fn, "rb").read()).decode() if os.path.getsize(fn) < 2_000_000 else ""}}
        return {"status": "error", "msg": "v4l2 capture failed on %s" % dev}
    return {"status": "error", "msg": "no capture source"}

def cmd_add_camera(cam_type="v4l2", device="/dev/video0", url="", cam_id=""):
    """monitor_add：添加一路摄像头（V4L2/RTSP）"""
    cam = {"id": cam_id or ("cam%d" % len(_cfg["cameras"])),
           "type": cam_type, "enabled": True,
           "width": _cfg.get("cameras") and _cfg["cameras"][0].get("width") or 640,
           "height": _cfg.get("cameras") and _cfg["cameras"][0].get("height") or 480,
           "fps": 10}
    if cam_type == "v4l2":
        cam["device"] = device
    elif cam_type == "rtsp":
        if not url:
            return {"status": "error", "msg": "rtsp need url"}
        cam["url"] = url; cam["frame_port"] = 8890 + len(_cfg["cameras"]); cam["http_port"] = 8891 + len(_cfg["cameras"])
    _cfg["cameras"].append(cam)
    save_cfg()
    return {"status": "ok", "data": cam}

# ---------- HTTP：预览 MJPEG + 状态 API（供 App/Web/Qt） ----------
class MonitorHTTP(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _json(self, obj, code=200):
        b = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path.startswith("/status"):
            self._json(cmd_status())
        elif self.path.startswith("/list"):
            self._json(cmd_list())
        elif self.path.startswith("/snapshot"):
            q = self.path.split("?")[1] if "?" in self.path else ""
            cid = "cam0"
            for kv in q.split("&"):
                if kv.startswith("camera="): cid = kv.split("=")[1]
            r = cmd_snapshot(cid)
            if r.get("status") == "ok" and r.get("data", {}).get("b64"):
                import base64
                raw = base64.b64decode(r["data"]["b64"])
                self.send_response(200); self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(raw))); self.end_headers(); self.wfile.write(raw)
            else:
                self._json(r, 400)
        else:
            self._json({"status": "error", "msg": "not found"}, 404)

def main():
    log_init()
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", action="store_true", help="scan v4l2 devices & exit")
    ap.add_argument("--http", type=int, default=0, help="status http port (0=off)")
    args = ap.parse_args()
    load_cfg()
    if args.scan:
        print(json.dumps(scan_v4l2(), ensure_ascii=False, indent=2)); return
    LOG.info("MonitorService started (LN-0.4.3) cameras=%d", len(_cfg["cameras"]))
    if args.http:
        HTTPServer(("0.0.0.0", args.http), MonitorHTTP).serve_forever()
    else:
        # 常驻：每 30s 打印状态（守护由 runtime/监督拉起）
        while _state["running"]:
            time.sleep(30)

if __name__ == "__main__":
    main()
