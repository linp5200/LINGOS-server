#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Registry Client (Python)
版本: LN-B-5.0.0.0
功能：通过 Unix Socket 与 C 侧注册表通信，实现注册/查询/列表/注销
"""

import os
import sys
import json
import socket
import logging
import time
from typing import Dict, Any, Optional, List, Tuple

# ========== 日志配置 ==========
logger = logging.getLogger("RegistryClient")

# ========== 常量 ==========
REGISTRY_SOCKET_PATH = "/LINGOS/run/registry.sock"
DEFAULT_TIMEOUT = 30

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def set_registry_language(lang: str):
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang

def load_registry_language():
    global _current_lang
    try:
        config_path = "/LINGOS/system/config/ai_config.json"
        if os.path.exists(config_path):
            with open(config_path, "r") as f:
                cfg = json.load(f)
            lang = cfg.get("language", "en")
            if lang in ("en", "zh"):
                _current_lang = lang
    except:
        pass

load_registry_language()

# =============================================================
# 注册表客户端类
# =============================================================

class RegistryClient:
    """Python 注册表客户端（与 C 侧 registry.c 通信）"""

    def __init__(self, socket_path: str = REGISTRY_SOCKET_PATH):
        self.socket_path = socket_path
        self._connected = False
        logger.debug(f"RegistryClient initialized with socket: {self.socket_path}")

    def _connect(self) -> Optional[socket.socket]:
        """建立 Unix Socket 连接"""
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(DEFAULT_TIMEOUT)
            sock.connect(self.socket_path)
            self._connected = True
            return sock
        except FileNotFoundError:
            logger.error(f"Registry socket not found: {self.socket_path}")
            return None
        except ConnectionRefusedError:
            logger.error(f"Registry connection refused: {self.socket_path}")
            return None
        except Exception as e:
            logger.error(f"Registry connection error: {e}")
            return None

    def _send_recv(self, request: Dict) -> Tuple[bool, Dict]:
        """
        发送请求并接收响应
        :param request: 请求字典
        :return: (成功标志, 响应字典)
        """
        sock = self._connect()
        if not sock:
            return False, {"status": "error", "message": "Cannot connect to registry"}

        try:
            msg = json.dumps(request) + "\n"
            sock.send(msg.encode())

            # 读取响应（JSON 行）
            resp_data = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                resp_data += chunk
                if b"\n" in resp_data:
                    break

            sock.close()
            self._connected = False

            if not resp_data:
                return False, {"status": "error", "message": "Empty response"}

            try:
                resp = json.loads(resp_data.decode())
            except json.JSONDecodeError as e:
                logger.error(f"JSON decode error: {e}")
                return False, {"status": "error", "message": f"Invalid JSON: {e}"}

            return True, resp

        except socket.timeout:
            logger.error("Registry request timeout")
            return False, {"status": "error", "message": "Timeout"}
        except Exception as e:
            logger.error(f"Registry request error: {e}")
            return False, {"status": "error", "message": str(e)}

    # ====== 核心 API ======

    def register(self, entry: Dict) -> bool:
        """
        注册新条目
        :param entry: 条目字典，必须包含 id 和 type
        :return: 成功 True，失败 False
        """
        request = {
            "cmd": "register",
            "entry": entry
        }
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            logger.info(f"Registered: {entry.get('id')}")
            return True
        else:
            logger.error(f"Register failed: {resp.get('message')}")
            return False

    def unregister(self, entry_id: str) -> bool:
        """
        注销条目
        :param entry_id: 条目 ID
        :return: 成功 True，失败 False
        """
        request = {
            "cmd": "unregister",
            "id": entry_id
        }
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            logger.info(f"Unregistered: {entry_id}")
            return True
        else:
            logger.error(f"Unregister failed: {resp.get('message')}")
            return False

    def get(self, entry_id: str) -> Optional[Dict]:
        """
        获取条目
        :param entry_id: 条目 ID
        :return: 条目字典，未找到返回 None
        """
        request = {
            "cmd": "get",
            "id": entry_id
        }
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            return resp.get("entry")
        else:
            logger.debug(f"Get failed for {entry_id}: {resp.get('message')}")
            return None

    def list(self, entry_type: Optional[str] = None) -> List[Dict]:
        """
        列出条目
        :param entry_type: 可选类型过滤（module/component/config/feature/skill/plugin/hook/selfcheck）
        :return: 条目列表
        """
        request = {
            "cmd": "list",
            "type": entry_type or ""
        }
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            return resp.get("entries", [])
        else:
            logger.error(f"List failed: {resp.get('message')}")
            return []

    def query(self, query_str: str) -> List[Dict]:
        """
        模糊查询
        :param query_str: 查询字符串（匹配 id 或 name）
        :return: 匹配的条目列表
        """
        request = {
            "cmd": "query",
            "query": query_str
        }
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            return resp.get("entries", [])
        else:
            logger.error(f"Query failed: {resp.get('message')}")
            return []

    def reload(self) -> bool:
        """
        热重载注册表
        :return: 成功 True，失败 False
        """
        request = {"cmd": "reload"}
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            logger.info("Registry reloaded")
            return True
        else:
            logger.error(f"Reload failed: {resp.get('message')}")
            return False

    def save(self) -> bool:
        """
        持久化保存注册表
        :return: 成功 True，失败 False
        """
        request = {"cmd": "save"}
        success, resp = self._send_recv(request)
        if success and resp.get("status") == "ok":
            logger.debug("Registry saved")
            return True
        else:
            logger.error(f"Save failed: {resp.get('message')}")
            return False

    def health(self) -> bool:
        """
        健康检查
        :return: 注册表可用 True，不可用 False
        """
        request = {"cmd": "ping"}
        success, resp = self._send_recv(request)
        return success and resp.get("status") == "ok"


# =============================================================
# 全局单例
# =============================================================

_client = None

def get_registry_client() -> RegistryClient:
    """获取全局注册表客户端实例"""
    global _client
    if _client is None:
        _client = RegistryClient()
    return _client


# =============================================================
# 便捷函数
# =============================================================

def registry_register(entry: Dict) -> bool:
    """便捷：注册条目"""
    return get_registry_client().register(entry)

def registry_unregister(entry_id: str) -> bool:
    """便捷：注销条目"""
    return get_registry_client().unregister(entry_id)

def registry_get(entry_id: str) -> Optional[Dict]:
    """便捷：获取条目"""
    return get_registry_client().get(entry_id)

def registry_list(entry_type: Optional[str] = None) -> List[Dict]:
    """便捷：列出条目"""
    return get_registry_client().list(entry_type)

def registry_query(query_str: str) -> List[Dict]:
    """便捷：模糊查询"""
    return get_registry_client().query(query_str)

def registry_reload() -> bool:
    """便捷：热重载"""
    return get_registry_client().reload()

def registry_health() -> bool:
    """便捷：健康检查"""
    return get_registry_client().health()


# =============================================================
# 自测
# =============================================================

if __name__ == "__main__":
    print("Testing Registry Client...")
    client = get_registry_client()

    # 测试健康检查
    if client.health():
        print("✓ Registry is available")
    else:
        print("✗ Registry is not available")

    # 测试列表
    skills = client.list("skill")
    print(f"Found {len(skills)} skills")
    for s in skills[:3]:
        print(f"  - {s.get('id')}: {s.get('name')}")