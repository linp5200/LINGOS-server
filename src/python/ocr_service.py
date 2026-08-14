#!/usr/bin/env python3
"""【0.2.2 vision】OCR 双引擎服务（先生裁决 2026-08-14：PaddleOCR + Tesseract 都用——体积不敏感）
- PaddleOCR：中文识别强（首选）
- Tesseract：轻量兜底（PaddleOCR 不可用时降级）
- 接入：yolo_service 同级——本地 socket 服务，visiond 检测帧 → OCR 请求
"""
import argparse
import io
import json
import logging
import os
import socket
import threading
import time

logging.basicConfig(level=logging.INFO, format="%(asctime)s [OCR] %(message)s")
log = logging.getLogger("OCR")

_paddle = None
_tess = None
_paddle_ready = False
_tess_ready = False


def init_engines():
    """初始化双引擎（失败静默——另一个兜底）"""
    global _paddle, _tess, _paddle_ready, _tess_ready
    # PaddleOCR（首选——中文强）
    try:
        from paddleocr import PaddleOCR
        _paddle = PaddleOCR(use_angle_cls=True, lang="ch", show_log=False)
        _paddle_ready = True
        log.info("PaddleOCR 就绪")
    except Exception as e:
        log.warning("PaddleOCR 不可用: %s（Tesseract 兜底）", str(e)[:80])
    # Tesseract（兜底）
    try:
        import pytesseract
        _tess = pytesseract
        _tess_ready = True
        log.info("Tesseract 就绪")
    except Exception as e:
        log.warning("Tesseract 不可用: %s", str(e)[:80])


def ocr_image(image_data: bytes) -> list:
    """OCR 识别：返回 [{text, confidence, box}]——PaddleOCR 优先，Tesseract 兜底"""
    results = []
    if _paddle_ready:
        try:
            import numpy as np
            from PIL import Image
            img = Image.open(io.BytesIO(image_data))
            arr = np.array(img)
            res = _paddle.ocr(arr, cls=True)
            for line in (res[0] if res else []):
                if not line:
                    continue
                box, (text, conf) = line
                if text and conf:
                    results.append({
                        "text": str(text),
                        "confidence": float(conf),
                        "box": [[float(p[0]), float(p[1])] for p in box],
                        "engine": "paddle",
                    })
            if results:
                return results
        except Exception as e:
            log.warning("PaddleOCR 识别失败: %s——Tesseract 兜底", str(e)[:80])
    if _tess_ready:
        try:
            from PIL import Image
            img = Image.open(io.BytesIO(image_data))
            data = _tess.image_to_data(img, lang="chi_sim+eng", output_type=_tess.Output.DICT)
            n = len(data.get("text", []))
            for i in range(n):
                text = (data["text"][i] or "").strip()
                conf = float(data["conf"][i] or 0)
                if text and conf > 30:
                    results.append({
                        "text": text,
                        "confidence": conf / 100.0,
                        "box": [[data["left"][i], data["top"][i]],
                                [data["left"][i] + data["width"][i], data["top"][i] + data["height"][i]]],
                        "engine": "tesseract",
                    })
        except Exception as e:
            log.warning("Tesseract 识别失败: %s", str(e)[:80])
    return results


def handle_client(conn):
    try:
        # 帧长度(4B) + JPEG 帧 → OCR → JSON 结果
        raw = conn.recv(4)
        if len(raw) != 4:
            return
        import struct
        n = struct.unpack(">I", raw)[0]
        if n <= 0 or n > 10 * 1024 * 1024:
            return
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                return
            data += chunk
        results = ocr_image(data)
        conn.sendall((json.dumps(results, ensure_ascii=False) + "\n").encode())
    except Exception as e:
        log.warning("OCR 客户端处理异常: %s", str(e)[:80])
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8892)
    args = ap.parse_args()
    init_engines()
    if not _paddle_ready and not _tess_ready:
        log.error("双引擎均不可用——OCR 服务无法启动")
        return
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(5)
    log.info("OCR 服务监听 127.0.0.1:%d（PaddleOCR=%s Tesseract=%s）",
             args.port, _paddle_ready, _tess_ready)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle_client, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
