#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS System Call Client
版本: LN-B-4.2.0.0
功能：通过Unix Socket调用C端原子API，含记忆操作、超时重试、编码修复
"""

import os
import sys
import json
import socket
import logging
import time
from typing import Dict, Any, Tuple, Optional

# ========== 日志配置 ==========
logger = logging.getLogger("SyscallClient")

# ========== 常量 ==========
DAEMON_SOCKET_PATH = "/LINGOS/run/daemon.sock"
DEFAULT_TIMEOUT = 30
MAX_FILE_SIZE_BYTES = 10 * 1024 * 1024  # 10MB 文件大小限制

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

# ========== 核心调用 ==========
def call_syscall(operation: str, args: Dict[str, Any], timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    """
    调用C端原子系统调用
    :param operation: 操作名称 (如 "file_read")
    :param args: 参数字典
    :param timeout: 超时秒数
    :return: (成功标志, 结果字符串)
    """
    logger.debug(f"call_syscall: Enter, operation={operation}, args={args}, timeout={timeout}")
    max_retries = 2
    last_error = ""

    for attempt in range(max_retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            sock.connect(DAEMON_SOCKET_PATH)

            request = {
                "cmd": "syscall",
                "operation": operation,
                "args": args
            }
            msg = json.dumps(request) + "\n"
            logger.debug(f"Sending syscall: {operation} args={args}")
            sock.send(msg.encode())

            # 读取响应
            resp_data = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                resp_data += chunk
                if b"\n" in resp_data:
                    break
            sock.close()

            if not resp_data:
                return False, t("Empty response from daemon", "守护进程返回空响应")

            # ====== 修复 E3/E4：使用 surrogateescape 处理非 UTF-8 字节 ======
            try:
                decoded = resp_data.decode('utf-8')
            except UnicodeDecodeError:
                logger.warning(f"call_syscall: UTF-8 decode failed, using surrogateescape for {operation}")
                decoded = resp_data.decode('utf-8', errors='surrogateescape')

            logger.debug(f"call_syscall: Response decoded (len={len(decoded)})")

            try:
                resp = json.loads(decoded)
            except json.JSONDecodeError as e:
                logger.error(f"JSON decode error: {e}, data preview: {decoded[:200]}")
                # 尝试修复：如果数据包含控制字符，使用更宽松的解析
                import re
                # 移除控制字符（保留换行和制表符）
                cleaned = re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]', '', decoded)
                try:
                    resp = json.loads(cleaned)
                    logger.info(f"call_syscall: JSON recovered after cleaning control chars")
                except json.JSONDecodeError as e2:
                    logger.error(f"call_syscall: JSON recovery failed: {e2}")
                    return False, t(f"Invalid response from daemon: {e}", f"守护进程响应格式错误：{e}")

            if resp.get("status") == "ok":
                data = resp.get("data")
                # ====== 文件大小限制 ======
                if operation in ("file_read", "file_write") and isinstance(data, str):
                    if len(data) > MAX_FILE_SIZE_BYTES:
                        logger.warning(f"call_syscall: File size {len(data)} exceeds limit, truncating")
                        data = data[:MAX_FILE_SIZE_BYTES] + "\n... [TRUNCATED: file too large]"
                if isinstance(data, str):
                    return True, data
                else:
                    return True, json.dumps(data, ensure_ascii=False)
            else:
                error_type = resp.get("error_type", "unknown")
                message = resp.get("message", t("Unknown error", "未知错误"))
                logger.warning(f"call_syscall: Error [{error_type}]: {message}")
                return False, f"{t('Error', '错误')} [{error_type}]: {message}"

        except socket.timeout:
            last_error = t(f"Timeout after {timeout}s (attempt {attempt+1})", f"超时 {timeout} 秒（尝试 {attempt+1}）")
            logger.warning(last_error)
            if attempt < max_retries - 1:
                time.sleep(1)
                continue
            return False, last_error
        except ConnectionRefusedError:
            last_error = t("Daemon not reachable", "守护进程不可达")
            logger.warning(last_error)
            if attempt < max_retries - 1:
                time.sleep(2)
                continue
            return False, last_error
        except json.JSONDecodeError as e:
            logger.error(f"JSON decode error: {e}")
            return False, t(f"Invalid response from daemon: {e}", f"守护进程响应格式错误：{e}")
        except Exception as e:
            logger.error(f"Syscall exception: {e}")
            last_error = str(e)
            if attempt < max_retries - 1:
                time.sleep(1)
                continue
            return False, str(e)

    return False, last_error


# =============================================================
# 文件操作便捷函数
# =============================================================

def file_read(path: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    logger.debug(f"file_read: path={path}")
    return call_syscall("file_read", {"path": path}, timeout)

def file_write(path: str, content: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    logger.debug(f"file_write: path={path}, content_len={len(content)}")
    return call_syscall("file_write", {"path": path, "content": content}, timeout)

def file_delete(path: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("file_delete", {"path": path}, timeout)

def file_list(path: str = ".", timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("file_list", {"path": path}, timeout)

def file_mkdir(path: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("file_mkdir", {"path": path}, timeout)


# =============================================================
# 记忆操作便捷函数
# =============================================================

def memory_write(mem_type: str, content: str, keywords: list = None, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    if keywords is None:
        keywords = []
    return call_syscall("memory_write", {
        "type": mem_type,
        "content": content,
        "keywords": keywords
    }, timeout)

def memory_search(keyword: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("memory_search", {"keyword": keyword}, timeout)

def memory_read(mem_id: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("memory_read", {"id": mem_id}, timeout)

def memory_delete(mem_id: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("memory_delete", {"id": mem_id}, timeout)

def memory_index(mem_type: Optional[str] = None, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    args = {}
    if mem_type:
        args["type"] = mem_type
    return call_syscall("memory_index", args, timeout)


# =============================================================
# 进程操作便捷函数
# =============================================================

def process_list(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("process_list", {}, timeout)

def process_kill(pid: int, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("process_kill", {"pid": pid}, timeout)

def process_info(pid: int, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("process_info", {"pid": pid}, timeout)


# =============================================================
# 系统信息便捷函数
# =============================================================

def system_info(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("system_info", {}, timeout)

def system_uptime(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("system_uptime", {}, timeout)

def system_memory(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("system_memory", {}, timeout)

def system_disk(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("system_disk", {}, timeout)

def system_cpu(timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("system_cpu", {}, timeout)


# =============================================================
# 网络操作便捷函数
# =============================================================

def net_ping(host: str = "8.8.8.8", count: int = 4, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("net_ping", {"host": host, "count": count}, timeout)

def net_curl(url: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("net_curl", {"url": url}, timeout)

def net_dns_lookup(domain: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("net_dns_lookup", {"domain": domain}, timeout)


# =============================================================
# 命令执行（高风险）
# =============================================================

def exec_command(command: str, timeout: int = DEFAULT_TIMEOUT) -> Tuple[bool, str]:
    return call_syscall("exec_command", {"command": command}, timeout)


# =============================================================
# 健康检查
# =============================================================

def daemon_health(timeout: int = 5) -> bool:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect(DAEMON_SOCKET_PATH)
        sock.send(b'{"cmd":"ping"}\n')
        resp = sock.recv(1024)
        sock.close()
        return b'"pong"' in resp
    except:
        return False


# =============================================================
# 语言设置
# =============================================================

def set_language(lang: str):
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang

def get_language() -> str:
    return _current_lang