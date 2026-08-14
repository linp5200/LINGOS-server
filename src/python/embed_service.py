#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Embedding Service
版本: LN-B-4.2.0.0
功能：本地文本向量化服务（sentence-transformers）
"""

import os
import sys
import json
import socket
import threading
import time
import logging
import signal
import numpy as np
from typing import List, Optional

# ========== 日志配置 ==========
LOG_DIR = "/LINGOS/Debug"
os.makedirs(LOG_DIR, exist_ok=True)

log_file = os.path.join(LOG_DIR, "embed_service.log")
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
    handlers=[
        logging.FileHandler(log_file),
        logging.StreamHandler(sys.stderr)
    ]
)
logger = logging.getLogger("EmbedService")
logger.info("=== Embedding Service starting (LN-B-4.2.0.0) ===")

# ========== 常量 ==========
SOCKET_PATH = "/LINGOS/run/embed.sock"
MODEL_NAME = "sentence-transformers/all-MiniLM-L6-v2"
FALLBACK_MODEL = "all-MiniLM-L6-v2"
VECTOR_DIM = 384
PARENT_PID = os.getppid()

# ========== 模型加载 ==========
_model = None
_model_loaded = False

def load_model():
    """加载 sentence-transformers 模型"""
    global _model, _model_loaded

    if _model_loaded:
        return True

    logger.info(f"Loading embedding model: {MODEL_NAME}")

    try:
        from sentence_transformers import SentenceTransformer
        _model = SentenceTransformer(MODEL_NAME)
        _model_loaded = True
        logger.info(f"Model loaded successfully (dim={_model.get_sentence_embedding_dimension()})")
        return True
    except ImportError as e:
        logger.error(f"sentence-transformers not installed: {e}")
        logger.info("Install with: pip install sentence-transformers")
        return False
    except Exception as e:
        logger.error(f"Failed to load model: {e}")
        return False

def get_embedding(text: str) -> Optional[List[float]]:
    """获取文本向量"""
    global _model

    if not _model_loaded:
        if not load_model():
            return None

    try:
        embedding = _model.encode(text, normalize_embeddings=True)
        return embedding.tolist()
    except Exception as e:
        logger.error(f"Embedding error: {e}")
        return None

# ========== Socket 服务器 ==========
def handle_client(conn, addr):
    """处理客户端连接"""
    try:
        data = b""
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break

        if not data:
            conn.close()
            return

        req = json.loads(data.decode())
        cmd = req.get("cmd")
        logger.debug(f"Received cmd: {cmd}")

        if cmd == "embed":
            text = req.get("text", "")
            if not text:
                resp = {"status": "error", "error": "Missing 'text'"}
                conn.send((json.dumps(resp) + "\n").encode())
                conn.close()
                return

            logger.debug(f"Embedding: text_len={len(text)}")

            embedding = get_embedding(text)
            if embedding is None:
                resp = {"status": "error", "error": "Failed to generate embedding"}
                conn.send((json.dumps(resp) + "\n").encode())
                conn.close()
                return

            resp = {
                "status": "ok",
                "embedding": embedding,
                "dim": len(embedding)
            }
            conn.send((json.dumps(resp) + "\n").encode())
            logger.debug(f"Embedding sent (dim={len(embedding)})")

        elif cmd == "ping":
            resp = {"status": "ok", "content": "pong"}
            conn.send((json.dumps(resp) + "\n").encode())

        elif cmd == "health":
            resp = {
                "status": "ok",
                "model_loaded": _model_loaded,
                "dim": VECTOR_DIM if _model_loaded else 0
            }
            conn.send((json.dumps(resp) + "\n").encode())

        else:
            resp = {"status": "error", "error": f"Unknown command: {cmd}"}
            conn.send((json.dumps(resp) + "\n").encode())

    except json.JSONDecodeError as e:
        logger.error(f"JSON decode error: {e}")
        try:
            conn.send(json.dumps({"status": "error", "error": str(e)}).encode() + b"\n")
        except:
            pass
    except Exception as e:
        logger.error(f"Handle client error: {e}")
        try:
            conn.send(json.dumps({"status": "error", "error": str(e)}).encode() + b"\n")
        except:
            pass
    finally:
        conn.close()

def start_server():
    """启动 Unix Socket 服务"""
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o666)
    server.listen(5)

    logger.info(f"Embedding service listening on {SOCKET_PATH}")

    while True:
        try:
            conn, addr = server.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            logger.error(f"Accept error: {e}")
            time.sleep(1)

def monitor_parent():
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

def signal_handler(sig, frame):
    logger.info(f"Received signal {sig}, shutting down...")
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    sys.exit(0)

# ========== 主函数 ==========
def main():
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # 父进程监控
    threading.Thread(target=monitor_parent, daemon=True).start()

    # 预加载模型
    if not load_model():
        logger.warning("Model not loaded, falling back to random embeddings")
        logger.warning("Install sentence-transformers: pip install sentence-transformers")

    # 启动服务
    start_server()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        logger.error(f"Fatal error: {e}")
        sys.exit(1)