#!/usr/bin/env python3
"""【0.2.2 vision】坐标标定服务（先生裁决 2026-08-14 双模式）
- 自动模式：检测到已知尺寸物体（如桌子两角）→ 用户输入两点实测距离 → 创建平面比例（像素→厘米）
- 手动模式：二维码放置指定区域 → 计算创建三维坐标
- 产出：/LINGOS/data/vision/calibration.json（visiond spatial_mapper 读取——骨架已有）
"""
import argparse
import json
import logging
import os
import socket
import threading

logging.basicConfig(level=logging.INFO, format="%(asctime)s [CALIB] %(message)s")
log = logging.getLogger("CALIB")

CALIB_PATH = "/LINGOS/data/vision/calibration.json"


def load_calib():
    try:
        if os.path.exists(CALIB_PATH):
            with open(CALIB_PATH) as f:
                return json.load(f)
    except Exception:
        pass
    return {}


def save_calib(data):
    os.makedirs(os.path.dirname(CALIB_PATH), exist_ok=True)
    with open(CALIB_PATH, "w") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


# ---------- 自动模式：平面比例 ----------
def auto_calibrate(p1, p2, real_distance_cm):
    """两点像素坐标 + 实测距离 → 每像素厘米比例（平面映射）
    p1/p2: [x, y] 像素（如桌子两角）"""
    dx = p2[0] - p1[0]
    dy = p2[1] - p1[1]
    pixel_dist = (dx * dx + dy * dy) ** 0.5
    if pixel_dist <= 0:
        return {"error": "两点重合——无法计算比例"}
    scale = real_distance_cm / pixel_dist  # cm/像素
    calib = load_calib()
    calib.update({
        "mode": "planar",
        "scale_cm_per_px": scale,
        "reference_points": {"p1": p1, "p2": p2, "real_distance_cm": real_distance_cm},
        "updated": __import__("time").time(),
    })
    save_calib(calib)
    return {"status": "ok", "scale_cm_per_px": scale}


def px_to_cm(x, y):
    """像素 → 世界坐标（厘米）——平面映射"""
    calib = load_calib()
    scale = calib.get("scale_cm_per_px")
    if not scale:
        return None
    # 以参考点 p1 为原点
    p1 = calib.get("reference_points", {}).get("p1", [0, 0])
    return {"world_x": (x - p1[0]) * scale, "world_y": (y - p1[1]) * scale}


# ---------- 手动模式：二维码三维标定 ----------
def manual_calibrate(markers):
    """二维码放置已知位置 → 记录其像素坐标与三维坐标映射
    markers: [{id, pixel:[x,y], world:[x,y,z]}]
    用多个标记建立像素↔三维的仿射映射（简单模型——单目近似）"""
    if len(markers) < 3:
        return {"error": "至少需要 3 个标记点才能建立三维映射"}
    calib = load_calib()
    calib.update({
        "mode": "3d",
        "markers": markers,
        "updated": __import__("time").time(),
    })
    save_calib(calib)
    return {"status": "ok", "markers": len(markers)}


def handle_client(conn):
    try:
        req = conn.recv(4096).decode()
        msg = json.loads(req)
        cmd = msg.get("cmd", "")
        if cmd == "auto":
            r = auto_calibrate(msg["p1"], msg["p2"], float(msg["distance_cm"]))
            conn.sendall((json.dumps(r, ensure_ascii=False) + "\n").encode())
        elif cmd == "manual":
            r = manual_calibrate(msg["markers"])
            conn.sendall((json.dumps(r, ensure_ascii=False) + "\n").encode())
        elif cmd == "px_to_cm":
            r = px_to_cm(msg["x"], msg["y"])
            conn.sendall((json.dumps(r or {"error": "未标定"}, ensure_ascii=False) + "\n").encode())
        elif cmd == "get":
            conn.sendall((json.dumps(load_calib(), ensure_ascii=False) + "\n").encode())
        else:
            conn.sendall((json.dumps({"error": "unknown cmd"}, ensure_ascii=False) + "\n").encode())
    except Exception as e:
        try:
            conn.sendall((json.dumps({"error": str(e)}, ensure_ascii=False) + "\n").encode())
        except Exception:
            pass
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8893)
    args = ap.parse_args()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(5)
    log.info("标定服务监听 127.0.0.1:%d（当前标定: %s）", args.port, load_calib().get("mode", "无"))
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle_client, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
