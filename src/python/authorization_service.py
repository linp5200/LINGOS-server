#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Authorization Service
版本: LN-B-5.0.0.0-rc0.4
功能：独立授权服务，处理高风险操作确认
      超时时间从 ai_config.json 读取，支持健康检查
      修改：traceback 导入修复；日志标准化；
           从 ai_config.json 读取 auth_timeout 配置
"""

import os
import sys
import json
import socket
import threading
import time
import logging
import traceback  # 【修复】显式导入 traceback
from typing import Dict, Optional

# ========== 常量 ==========
LOG_FILE = "/LINGOS/Debug/authorization.log"
SOCKET_PATH = "/LINGOS/run/auth.sock"
CONFIG_PATH = "/LINGOS/system/config/ai_config.json"
DEFAULT_TIMEOUT = 60
PARENT_PID = os.getppid()

# ========== 多语言支持 ==========
_current_lang = "en"

def load_language_preference() -> None:
    global _current_lang
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, "r") as f:
                cfg = json.load(f)
            lang = cfg.get("language", "en")
            if lang in ("en", "zh"):
                _current_lang = lang
                logger.info(f"Language preference loaded: {_current_lang}")
    except Exception as e:
        logger.warning(f"Failed to load language preference: {e}")

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

# ========== 日志配置 ==========
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler(sys.stderr)
    ]
)
logger = logging.getLogger("AuthService")
logger.info("=== Authorization Service starting (PID=%d) ===")

# ========== 全局状态 ==========
pending_requests: Dict[str, dict] = {}
request_id_counter = 0
lock = threading.RLock()
stop_flag = False
stats = {
    "total_requests": 0,
    "approved": 0,
    "denied": 0,
    "timeout": 0,
    "active": 0
}
TIMEOUT = DEFAULT_TIMEOUT  # 将从配置文件读取

def load_timeout_config() -> None:
    """从 ai_config.json 读取 auth_timeout"""
    global TIMEOUT
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, "r") as f:
                cfg = json.load(f)
            timeout = cfg.get("auth_timeout", DEFAULT_TIMEOUT)
            if timeout >= 10:
                TIMEOUT = timeout
                logger.info(f"Auth timeout set to: {TIMEOUT} seconds")
            else:
                TIMEOUT = DEFAULT_TIMEOUT
                logger.warning(f"Invalid auth_timeout value: {timeout}, using default: {DEFAULT_TIMEOUT}")
        else:
            TIMEOUT = DEFAULT_TIMEOUT
            logger.info(f"Config not found, using default auth timeout: {TIMEOUT}")
    except Exception as e:
        logger.warning(f"Failed to load auth_timeout from config: {e}, using default: {DEFAULT_TIMEOUT}")
        TIMEOUT = DEFAULT_TIMEOUT

# ========== 父进程监控 ==========
def monitor_parent() -> None:
    logger.debug("Parent monitor thread started, PARENT_PID=%d", PARENT_PID)
    while not stop_flag:
        try:
            os.kill(PARENT_PID, 0)
            time.sleep(5)
        except OSError:
            logger.info(t("Parent process terminated, exiting authorization service.",
                         "父进程已终止，授权服务退出。"))
            sys.exit(0)
        except Exception as e:
            logger.warning(f"Parent monitor error: {e}")
            time.sleep(5)
    logger.debug("Parent monitor thread exiting")

# ========== 授权请求处理 ==========
def generate_request_id() -> str:
    global request_id_counter
    with lock:
        request_id_counter += 1
        rid = f"auth_{request_id_counter}_{int(time.time())}"
        logger.debug(f"Generated request ID: {rid}")
        return rid

def timeout_request(rid: str) -> None:
    with lock:
        if rid in pending_requests and pending_requests[rid]["status"] == "pending":
            pending_requests[rid]["status"] = "denied"
            pending_requests[rid]["timeout_reason"] = "timeout"
            stats["timeout"] += 1
            logger.info(f"Request {rid} timed out after {TIMEOUT}s, automatically denied")
            sys.stdout.write(f"\n{t('[Auth] Request ', '[授权] 请求 ')}{rid}{t(' timed out, automatically denied', ' 超时，已自动拒绝')}\n")
            sys.stdout.flush()

def handle_client(conn: socket.socket, addr: tuple) -> None:
    logger.debug(f"New client connection from {addr}")
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
            logger.debug("Empty data from client, closing")
            conn.close()
            return

        req_text = data.decode().strip()
        logger.debug(f"Received data: {req_text[:200]}...")

        try:
            req = json.loads(req_text)
        except json.JSONDecodeError as e:
            logger.error(f"JSON decode error: {e}, data={req_text[:100]}")
            conn.send(b'{"status":"error","message":"Invalid JSON"}\n')
            conn.close()
            return

        cmd = req.get("cmd")
        logger.info(f"Processing command: {cmd}")

        if cmd == "request":
            rid = generate_request_id()
            skill = req.get("skill", "unknown")
            args = req.get("args", {})
            session = req.get("session", "default")

            with lock:
                pending_requests[rid] = {
                    "skill": skill,
                    "args": args,
                    "session": session,
                    "status": "pending",
                    "timestamp": time.time(),
                    "timeout_reason": None
                }
                stats["total_requests"] += 1
                stats["active"] += 1

            sys.stdout.write(f"\n{t('[Auth] High-risk operation request: ', '[授权] 高风险操作请求：')}{skill}\n")
            sys.stdout.write(f"{t('  Parameters: ', '  参数：')}{json.dumps(args, ensure_ascii=False)[:200]}\n")
            sys.stdout.write(f"{t('  Session: ', '  会话：')}{session}\n")
            sys.stdout.write(f"{t('Please respond Y/N (timeout ', '请响应 Y/N（超时 ')}{TIMEOUT}{t('s): ', '秒）：')}")
            sys.stdout.flush()

            timer = threading.Timer(TIMEOUT, timeout_request, args=[rid])
            timer.daemon = True
            timer.start()
            with lock:
                pending_requests[rid]["timer"] = timer

            logger.info(f"Request created: {rid}, skill={skill}, session={session}")
            conn.send(json.dumps({"request_id": rid}).encode() + b"\n")

        elif cmd in ("approve", "reject"):
            # 【协议v3】App/Web 审批通道：auth_resp → websocket_server → auth.sock approve
            rid = req.get("request_id", "")
            approved = (cmd == "approve")
            reason = req.get("reason", "")
            with lock:
                info = pending_requests.get(rid)
                if info and info["status"] == "pending":
                    info["status"] = "approved" if approved else "denied"
                    info["approved_at"] = time.time()
                    info["reason"] = reason
                    if info.get("timer"):
                        try: info["timer"].cancel()
                        except Exception: pass
                    stats["approved" if approved else "denied"] += 1
                    stats["active"] = max(0, stats["active"] - 1)
                    sys.stdout.write(f"\n{t('[Auth] App decision: ', '[授权] App 已决定：')}{rid} -> {'approved' if approved else 'denied'}\n")
                    sys.stdout.flush()
                    conn.send(json.dumps({"status": "ok", "request_id": rid}).encode() + b"\n")
                else:
                    conn.send(json.dumps({"status": "error", "message": "request not found or already decided"}).encode() + b"\n")

        elif cmd == "pending":
            with lock:
                found = None
                for rid, info in pending_requests.items():
                    if info["status"] == "pending":
                        found = rid
                        break
                if found:
                    conn.send(json.dumps({"request_id": found}).encode() + b"\n")
                    logger.debug(f"Pending request found: {found}")
                else:
                    conn.send(b'{"request_id":""}\n')
                    logger.debug("No pending requests")

        elif cmd == "respond":
            rid = req.get("request_id")
            decision = req.get("decision")
            logger.info(f"Respond to request {rid}: decision={decision}")

            with lock:
                if rid in pending_requests:
                    if pending_requests[rid]["status"] == "pending":
                        timer = pending_requests[rid].get("timer")
                        if timer:
                            timer.cancel()
                            logger.debug(f"Timer cancelled for {rid}")

                        new_status = "approved" if decision else "denied"
                        pending_requests[rid]["status"] = new_status
                        stats["active"] -= 1
                        if decision:
                            stats["approved"] += 1
                        else:
                            stats["denied"] += 1

                        logger.info(f"Request {rid} decided: {new_status}")
                        conn.send(b'{"status":"ok"}\n')
                    else:
                        msg = t("Request already processed", "请求已处理")
                        logger.warning(f"Request {rid} already processed, current status={pending_requests[rid]['status']}")
                        conn.send(json.dumps({"status": "error", "message": msg}).encode() + b"\n")
                else:
                    msg = t("Request not found", "请求未找到")
                    logger.warning(f"Request {rid} not found")
                    conn.send(json.dumps({"status": "error", "message": msg}).encode() + b"\n")

        elif cmd == "query":
            rid = req.get("request_id")
            logger.debug(f"Query request {rid}")
            with lock:
                info = pending_requests.get(rid)
                if info:
                    status = info["status"]
                    response = {"status": status}
                    if status == "denied" and info.get("timeout_reason") == "timeout":
                        response["reason"] = "timeout"
                    conn.send(json.dumps(response).encode() + b"\n")
                    logger.debug(f"Query {rid}: status={status}")
                else:
                    conn.send(json.dumps({"status": "not_found", "message": t("Request not found", "请求未找到")}).encode() + b"\n")
                    logger.warning(f"Query {rid}: not found")

        elif cmd == "health":
            with lock:
                active = stats["active"]
                pending_count = sum(1 for info in pending_requests.values() if info["status"] == "pending")
                health = {
                    "status": "ok",
                    "active_requests": active,
                    "pending_requests": pending_count,
                    "total_requests": stats["total_requests"],
                    "approved": stats["approved"],
                    "denied": stats["denied"],
                    "timeout": stats["timeout"],
                    "uptime": int(time.time() - start_time)
                }
                conn.send(json.dumps(health).encode() + b"\n")
                logger.debug(f"Health check: active={active}, pending={pending_count}")

        elif cmd == "ping":
            conn.send(b'{"status":"ok","content":"pong"}\n')
            logger.debug("Ping received, pong sent")

        elif cmd == "stats":
            with lock:
                stats_copy = stats.copy()
                stats_copy["pending"] = sum(1 for info in pending_requests.values() if info["status"] == "pending")
                conn.send(json.dumps(stats_copy).encode() + b"\n")
                logger.debug("Stats requested")

        else:
            msg = t(f"Unknown command: {cmd}", f"未知命令：{cmd}")
            logger.warning(f"Unknown command: {cmd}")
            conn.send(json.dumps({"status": "error", "message": msg}).encode() + b"\n")

    except Exception as e:
        logger.error(f"Handle client error: {e}")
        logger.error(traceback.format_exc())  # 【修复】现在 traceback 已导入
        try:
            conn.send(json.dumps({"status": "error", "message": str(e)}).encode() + b"\n")
        except:
            pass
    finally:
        conn.close()
        logger.debug("Client connection closed")

# ========== 服务器主循环 ==========
start_time = 0

def start_server() -> None:
    global stop_flag, start_time, TIMEOUT
    start_time = time.time()

    # 加载 auth_timeout 配置
    load_timeout_config()

    if os.path.exists(SOCKET_PATH):
        logger.info(f"Removing stale socket: {SOCKET_PATH}")
        os.unlink(SOCKET_PATH)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        server.bind(SOCKET_PATH)
        os.chmod(SOCKET_PATH, 0o666)
        server.listen(5)
        logger.info(f"Authorization service listening on {SOCKET_PATH}")
    except Exception as e:
        logger.error(f"Failed to bind socket: {e}")
        sys.exit(1)

    monitor_thread = threading.Thread(target=monitor_parent, daemon=True)
    monitor_thread.start()
    logger.debug("Parent monitor thread started")

    def stats_printer():
        while not stop_flag:
            time.sleep(300)
            with lock:
                logger.info(f"Stats: total={stats['total_requests']}, "
                           f"approved={stats['approved']}, denied={stats['denied']}, "
                           f"timeout={stats['timeout']}, active={stats['active']}, "
                           f"pending={sum(1 for info in pending_requests.values() if info['status'] == 'pending')}")
    threading.Thread(target=stats_printer, daemon=True).start()

    logger.info("Authorization service ready, waiting for connections...")

    while not stop_flag:
        try:
            conn, addr = server.accept()
            logger.debug(f"Accepted connection from {addr}")
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            if not stop_flag:
                logger.error(f"Accept error: {e}")
                time.sleep(1)

    server.close()
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    logger.info(t("Authorization service stopped", "授权服务已停止"))

# ========== 信号处理 ==========
def signal_handler(sig, frame) -> None:
    global stop_flag
    logger.info(t("Received signal, shutting down...", "收到信号，正在关闭..."))
    stop_flag = True
    time.sleep(1)
    sys.exit(0)

# ========== 主入口 ==========
if __name__ == "__main__":
    import signal

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    load_language_preference()

    logger.info(t("LING OS Authorization Service starting", "LING OS 授权服务启动中"))
    start_server()