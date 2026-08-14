#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS YOLO Inference Service
版本: LN-B-5.0.0.0
功能：YOLOv8 物体检测推理服务，通过 Unix Socket 接收图像并返回检测结果
      由 lingos_visiond fork 启动
"""

import os
import sys
import json
import socket
import threading
import time
import logging
import signal
import base64
import numpy as np
from typing import List, Dict, Any, Optional, Tuple
from PIL import Image
import io

# ========== 日志配置 ==========
LOG_DIR = "/LINGOS/Debug"
os.makedirs(LOG_DIR, exist_ok=True)

log_file = os.path.join(LOG_DIR, "yolo_service.log")
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
    handlers=[
        logging.FileHandler(log_file),
        logging.StreamHandler(sys.stderr)
    ]
)
logger = logging.getLogger("YOLOService")
logger.info("=== YOLO Service starting (LN-B-5.0.0.0) ===")

# ========== 常量 ==========
SOCKET_PATH = "/LINGOS/run/yolo.sock"
MODEL_PATH = "/LINGOS/models/yolov8n.pt"
DEFAULT_CONFIDENCE = 0.5
PARENT_PID = os.getppid()

# ========== 模型加载 ==========
_model = None
_model_loaded = False
_model_load_error = None

def load_model() -> bool:
    """加载 YOLO 模型"""
    global _model, _model_loaded, _model_load_error

    if _model_loaded:
        return True

    logger.info(f"Loading YOLO model from: {MODEL_PATH}")

    try:
        from ultralytics import YOLO

        if not os.path.exists(MODEL_PATH):
            logger.warning(f"Model not found at {MODEL_PATH}, attempting to download...")
            # 自动下载
            _model = YOLO("yolov8n.pt")
            # 保存到固定路径
            _model.save(MODEL_PATH)
            logger.info(f"Model downloaded and saved to {MODEL_PATH}")
        else:
            _model = YOLO(MODEL_PATH)

        _model_loaded = True
        logger.info(f"YOLO model loaded successfully")
        return True

    except ImportError as e:
        error_msg = f"ultralytics not installed: {e}"
        logger.error(error_msg)
        _model_load_error = error_msg
        return False
    except Exception as e:
        error_msg = f"Failed to load model: {e}"
        logger.error(error_msg)
        _model_load_error = error_msg
        return False

def run_inference(image_data: bytes, confidence: float = DEFAULT_CONFIDENCE) -> List[Dict]:
    """
    执行推理
    :param image_data: 图像数据（JPEG/PNG 格式）
    :param confidence: 置信度阈值
    :return: 检测结果列表
    """
    global _model

    if not _model_loaded:
        if not load_model():
            return [{"error": _model_load_error or "Model not loaded"}]

    try:
        # 解码图像
        img = Image.open(io.BytesIO(image_data))

        # 执行推理
        results = _model(img, conf=confidence)

        detections = []
        if results and len(results) > 0:
            boxes = results[0].boxes
            if boxes is not None:
                for box in boxes:
                    xyxy = box.xyxy[0].tolist()
                    conf = float(box.conf[0])
                    cls_id = int(box.cls[0])
                    label = _model.names[cls_id] if cls_id in _model.names else f"class_{cls_id}"

                    detections.append({
                        "x": int(xyxy[0]),
                        "y": int(xyxy[1]),
                        "width": int(xyxy[2] - xyxy[0]),
                        "height": int(xyxy[3] - xyxy[1]),
                        "class_id": cls_id,
                        "label": label,
                        "confidence": conf
                    })

        return detections

    except Exception as e:
        logger.error(f"Inference error: {e}")
        return [{"error": str(e)}]

# ========== Socket 服务器 ==========
def handle_client(conn: socket.socket, addr: Tuple) -> None:
    """处理客户端请求"""
    try:
        data = b""
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break

        if not data:
            conn.close()
            return

        req_text = data.decode().strip()
        try:
            req = json.loads(req_text)
        except json.JSONDecodeError as e:
            logger.error(f"JSON decode error: {e}")
            resp = {"status": "error", "error": f"Invalid JSON: {e}"}
            conn.send((json.dumps(resp) + "\n").encode())
            conn.close()
            return

        cmd = req.get("cmd")

        if cmd == "detect":
            image_b64 = req.get("image", "")
            confidence = req.get("threshold", DEFAULT_CONFIDENCE)

            if not image_b64:
                resp = {"status": "error", "error": "Missing 'image' field"}
                conn.send((json.dumps(resp) + "\n").encode())
                conn.close()
                return

            try:
                image_data = base64.b64decode(image_b64)
                logger.debug(f"Received image, size={len(image_data)} bytes")

                detections = run_inference(image_data, confidence)

                if detections and isinstance(detections[0], dict) and "error" in detections[0]:
                    resp = {"status": "error", "error": detections[0]["error"]}
                else:
                    resp = {
                        "status": "ok",
                        "detections": detections,
                        "count": len(detections)
                    }

                conn.send((json.dumps(resp) + "\n").encode())
                logger.debug(f"Sent {len(detections)} detections")

            except Exception as e:
                logger.error(f"Detection error: {e}")
                resp = {"status": "error", "error": str(e)}
                conn.send((json.dumps(resp) + "\n").encode())

        elif cmd == "ping":
            resp = {"status": "ok", "content": "pong"}
            conn.send((json.dumps(resp) + "\n").encode())

        elif cmd == "health":
            resp = {
                "status": "ok",
                "model_loaded": _model_loaded,
                "model_path": MODEL_PATH,
                "error": _model_load_error
            }
            conn.send((json.dumps(resp) + "\n").encode())

        elif cmd == "reload":
            global _model, _model_loaded, _model_load_error
            _model = None
            _model_loaded = False
            _model_load_error = None
            load_model()
            resp = {
                "status": "ok",
                "model_loaded": _model_loaded,
                "message": "Model reloaded"
            }
            conn.send((json.dumps(resp) + "\n").encode())

        else:
            resp = {"status": "error", "error": f"Unknown command: {cmd}"}
            conn.send((json.dumps(resp) + "\n").encode())

    except Exception as e:
        logger.error(f"Client handler error: {e}")
        try:
            conn.send(json.dumps({"status": "error", "error": str(e)}).encode() + b"\n")
        except:
            pass
    finally:
        conn.close()

def start_server() -> None:
    """启动 Unix Socket 服务"""
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o666)
    server.listen(5)

    logger.info(f"YOLO service listening on {SOCKET_PATH}")

    # 预加载模型
    load_model()

    while True:
        try:
            conn, addr = server.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            logger.error(f"Accept error: {e}")
            time.sleep(1)

def monitor_parent() -> None:
    """监控父进程，若父进程死亡则退出"""
    while True:
        time.sleep(5)
        try:
            os.kill(PARENT_PID, 0)
        except OSError:
            logger.warning("Parent process terminated, exiting")
            os._exit(0)
        except Exception as e:
            logger.warning(f"Parent monitor error: {e}")
            time.sleep(5)

def signal_handler(sig, frame) -> None:
    logger.info(f"Received signal {sig}, shutting down...")
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    sys.exit(0)

# ========== 主函数 ==========
def main():
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    threading.Thread(target=monitor_parent, daemon=True).start()

    try:
        start_server()
    except Exception as e:
        logger.error(f"Fatal error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()