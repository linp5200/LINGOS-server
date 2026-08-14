#!/usr/bin/env python3
"""【0.2.2 vision】RTSP 拉流服务——Python 端（先生裁决：RTSP 走 Python 拉流）
ffmpeg 拉 RTSP/MJPEG 流 → 逐帧 JPEG → 输出到：
  1. visiond 检测管线（本地 socket 帧通道）
  2. App 预览（HTTP MJPEG 服务）
用法：
  rtsp_streamer.py --url rtsp://localhost:8554/小方 --frame-port 8890 --http-port 8891
"""
import argparse
import io
import json
import logging
import socket
import subprocess
import threading
import time

logging.basicConfig(level=logging.INFO, format="%(asctime)s [RTSP] %(message)s")
log = logging.getLogger("RTSP")

# 最新帧（供检测/预览消费）
_latest_frame = None
_latest_lock = threading.Lock()
_running = True


def ffmpeg_pull(url: str) -> None:
    """ffmpeg 拉流 → JPEG 帧（-f image2pipe）"""
    global _latest_frame
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-rtsp_transport", "tcp",
        "-i", url,
        "-vf", "scale=640:-1",
        "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "3", "-",
    ]
    while _running:
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
            # JPEG 帧：SOI FFD8 ... EOI FFD9 分割
            buf = b""
            while _running:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                buf += chunk
                # 按 JPEG EOI 分割完整帧
                while True:
                    start = buf.find(b"\xff\xd8")
                    end = buf.find(b"\xff\xd9", start + 2 if start >= 0 else 0)
                    if start < 0 or end < 0:
                        break
                    frame = buf[start:end + 2]
                    buf = buf[end + 2:]
                    with _latest_lock:
                        _latest_frame = frame
            proc.wait(timeout=5)
        except Exception as e:
            log.warning("ffmpeg 拉流中断: %s——5s 后重连", e)
            time.sleep(5)


def frame_server(port: int) -> None:
    """visiond 检测管线：本地 socket 帧通道（收到 REQUEST 发最新帧）"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    log.info("帧通道监听 127.0.0.1:%d", port)
    while _running:
        try:
            conn, _ = srv.accept()
            conn.settimeout(5)
            conn.sendall(b"RTSP-STREAM-1\n")
            while _running:
                req = conn.recv(64)
                if not req:
                    break
                with _latest_lock:
                    frame = _latest_frame
                if frame:
                    conn.sendall(len(frame).to_bytes(4, "big") + frame)
                else:
                    conn.sendall((0).to_bytes(4, "big"))
            conn.close()
        except Exception:
            pass


def http_mjpeg(port: int) -> None:
    """App 预览：HTTP MJPEG 流"""
    from http.server import BaseHTTPRequestHandler, HTTPServer
    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.end_headers()
            while _running:
                with _latest_lock:
                    frame = _latest_frame
                if frame:
                    try:
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + frame + b"\r\n")
                    except Exception:
                        break
                time.sleep(0.05)
        def log_message(self, *a):
            pass
    HTTPServer(("0.0.0.0", port), H).serve_forever()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="RTSP/MJPEG 流地址")
    ap.add_argument("--frame-port", type=int, default=8890, help="visiond 帧通道端口")
    ap.add_argument("--http-port", type=int, default=8891, help="App 预览 MJPEG 端口")
    args = ap.parse_args()

    log.info("启动 RTSP 拉流: %s", args.url)
    threading.Thread(target=ffmpeg_pull, args=(args.url,), daemon=True).start()
    threading.Thread(target=frame_server, args=(args.frame_port,), daemon=True).start()
    threading.Thread(target=http_mjpeg, args=(args.http_port,), daemon=True).start()
    try:
        while _running:
            time.sleep(1)
    except KeyboardInterrupt:
        _running = False


if __name__ == "__main__":
    main()
