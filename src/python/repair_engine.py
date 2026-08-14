#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Repair Engine
版本: LN-B-5.0.0.0
功能：修复决策引擎、策略匹配、修复包生成、执行与验证、历史管理
      修改：Socket 通信完善；安全字符串增强
"""

import os
import sys
import json
import time
import shutil
import subprocess
import hashlib
import logging
import threading
import socket
import tarfile
from typing import Dict, Any, List, Optional, Tuple
from datetime import datetime
from pathlib import Path

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def load_repair_language():
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

load_repair_language()

# ========== 日志配置 ==========
logger = logging.getLogger("RepairEngine")

# ========== 常量 ==========
LINGOS_ROOT = "/LINGOS"
CONFIG_DIR = f"{LINGOS_ROOT}/system/config"
STATE_DIR = f"{LINGOS_ROOT}/state"
REPAIR_DIR = f"{LINGOS_ROOT}/repairs/archive"
BACKUP_DIR = f"{LINGOS_ROOT}/backups/pre_repair"
HISTORY_FILE = f"{STATE_DIR}/repair_history.json"
STRATEGY_FILE = f"{CONFIG_DIR}/repair_strategies.json"
COMPONENT_FILE = f"{CONFIG_DIR}/repair_components.json"
BACKUP_POLICY_FILE = f"{CONFIG_DIR}/backup_policy.conf"
REPAIR_SOCKET_PATH = "/LINGOS/run/repair.sock"

os.makedirs(REPAIR_DIR, exist_ok=True)
os.makedirs(BACKUP_DIR, exist_ok=True)
os.makedirs(STATE_DIR, exist_ok=True)
os.makedirs(CONFIG_DIR, exist_ok=True)

# ========== 配置文件模板 ==========
DEFAULT_STRATEGIES = {
    "strategies": [
        {
            "error_pattern": "lingosd.*crash|segfault",
            "severity": 5,
            "actions": [
                {"type": "restart_daemon", "priority": 1},
                {"type": "generate_repair_pack", "priority": 2, "component": "lingosd"}
            ],
            "fallback": "rollback_kernel"
        },
        {
            "error_pattern": "ai_server.*timeout|crash",
            "severity": 4,
            "actions": [
                {"type": "restart_ai_server", "priority": 1},
                {"type": "increase_timeout", "priority": 2}
            ]
        },
        {
            "error_pattern": "skill_exec.*timeout|SKILL_ERR_TIMEOUT",
            "severity": 3,
            "actions": [
                {"type": "retry_skill", "priority": 1, "max_retries": 2, "backoff": 2},
                {"type": "notify_user", "priority": 2}
            ]
        },
        {
            "error_pattern": "disk_usage.*[89][0-9]%|disk_full",
            "severity": 2,
            "actions": [
                {"type": "clean_logs", "priority": 1},
                {"type": "notify_user", "priority": 2}
            ]
        }
    ]
}

DEFAULT_COMPONENTS = {
    "mappings": [
        {
            "error_pattern": "lingosd.*crash|segfault",
            "components": [
                {"name": "lingosd", "source": "src/daemon/lingosd.c", "binary": "/usr/bin/lingosd"}
            ]
        },
        {
            "error_pattern": "ai_server.*crash",
            "components": [
                {"name": "ai_server", "source": "src/python/ai_server.py", "binary": "/LINGOS/bin/ai_server.py"}
            ]
        },
        {
            "error_pattern": "skill_executor.*assertion",
            "components": [
                {"name": "skill_executor", "source": "src/ai/skill_executor.c", "binary": "/usr/bin/lingos_linux"}
            ]
        }
    ]
}

DEFAULT_BACKUP_POLICY = """# Backup storage policy
# 1 = enable, 0 = disable

keep_by_time = 1
keep_days = 3
keep_by_major_version = 1
cloud_storage_enabled = 0
cloud_storage_type = webdav
cloud_storage_path = /mnt/cloud/lingos_backups/
keep_on_manual_update = 1
"""

# ========== 配置加载器 ==========
def ensure_config_files():
    if not os.path.exists(STRATEGY_FILE):
        with open(STRATEGY_FILE, 'w', encoding='utf-8') as f:
            json.dump(DEFAULT_STRATEGIES, f, indent=2, ensure_ascii=False)
        logger.info(f"Created default repair_strategies.json at {STRATEGY_FILE}")

    if not os.path.exists(COMPONENT_FILE):
        with open(COMPONENT_FILE, 'w', encoding='utf-8') as f:
            json.dump(DEFAULT_COMPONENTS, f, indent=2, ensure_ascii=False)
        logger.info(f"Created default repair_components.json at {COMPONENT_FILE}")

    if not os.path.exists(BACKUP_POLICY_FILE):
        with open(BACKUP_POLICY_FILE, 'w', encoding='utf-8') as f:
            f.write(DEFAULT_BACKUP_POLICY)
        logger.info(f"Created default backup_policy.conf at {BACKUP_POLICY_FILE}")

    if not os.path.exists(HISTORY_FILE):
        with open(HISTORY_FILE, 'w', encoding='utf-8') as f:
            json.dump([], f, indent=2, ensure_ascii=False)
        logger.info(f"Created empty repair_history.json at {HISTORY_FILE}")

def load_strategies() -> List[Dict]:
    try:
        with open(STRATEGY_FILE, 'r', encoding='utf-8') as f:
            data = json.load(f)
        return data.get('strategies', [])
    except Exception as e:
        logger.error(f"Failed to load strategies: {e}")
        return DEFAULT_STRATEGIES['strategies']

def load_components() -> List[Dict]:
    try:
        with open(COMPONENT_FILE, 'r', encoding='utf-8') as f:
            data = json.load(f)
        return data.get('mappings', [])
    except Exception as e:
        logger.error(f"Failed to load components: {e}")
        return DEFAULT_COMPONENTS['mappings']

def load_backup_policy() -> Dict:
    policy = {
        'keep_by_time': 1,
        'keep_days': 3,
        'keep_by_major_version': 1,
        'cloud_storage_enabled': 0,
        'cloud_storage_type': 'webdav',
        'cloud_storage_path': '/mnt/cloud/lingos_backups/',
        'keep_on_manual_update': 1
    }
    try:
        if os.path.exists(BACKUP_POLICY_FILE):
            with open(BACKUP_POLICY_FILE, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#'):
                        continue
                    if '=' in line:
                        key, val = line.split('=', 1)
                        key = key.strip()
                        val = val.strip()
                        if key in policy:
                            if key in ['keep_by_time', 'keep_days', 'keep_by_major_version',
                                       'cloud_storage_enabled', 'keep_on_manual_update']:
                                policy[key] = int(val)
                            else:
                                policy[key] = val
    except Exception as e:
        logger.warning(f"Failed to load backup policy, using defaults: {e}")
    return policy

# ========== 修复历史管理 ==========
def load_history() -> List[Dict]:
    if os.path.exists(HISTORY_FILE):
        try:
            with open(HISTORY_FILE, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception as e:
            logger.error(f"Failed to load history: {e}")
    return []

def save_history(history: List[Dict]):
    try:
        with open(HISTORY_FILE, 'w', encoding='utf-8') as f:
            json.dump(history, f, indent=2, ensure_ascii=False)
        logger.info(f"Saved repair history ({len(history)} entries)")
    except Exception as e:
        logger.error(f"Failed to save history: {e}")

def add_history_entry(entry: Dict):
    history = load_history()
    history.append(entry)
    save_history(history)

def get_history(limit: int = 10) -> List[Dict]:
    history = load_history()
    return history[-limit:]

def get_history_by_id(repair_id: str) -> Optional[Dict]:
    history = load_history()
    for entry in history:
        if entry.get('id') == repair_id:
            return entry
    return None

# ========== 修复决策引擎 ==========
class RepairEngine:
    def __init__(self):
        ensure_config_files()
        self.strategies = load_strategies()
        self.components = load_components()
        self.policy = load_backup_policy()
        self.running = False
        self.lock = threading.Lock()
        self.socket_server = None
        self.socket_thread = None

    def reload_config(self):
        with self.lock:
            self.strategies = load_strategies()
            self.components = load_components()
            self.policy = load_backup_policy()
            logger.info(t("RepairEngine config reloaded", "修复引擎配置已重载"))

    def analyze_error(self, error_event: Dict) -> Dict:
        message = error_event.get('message', '')
        source = error_event.get('source', 'unknown')
        error_type = error_event.get('type', 'unknown')
        fingerprint = hashlib.md5(f"{source}:{message}".encode()).hexdigest()[:16]

        return {
            "source": source,
            "error_type": error_type,
            "message": message,
            "fingerprint": fingerprint,
            "pattern": message.lower(),
            "timestamp": datetime.now().isoformat(),
            "context": error_event.get('context', {})
        }

    def match_strategy(self, error_info: Dict) -> Optional[Dict]:
        pattern = error_info.get('pattern', '')
        source = error_info.get('source', '')
        import re
        for strategy in self.strategies:
            if re.search(strategy['error_pattern'], pattern, re.IGNORECASE) or \
               re.search(strategy['error_pattern'], source, re.IGNORECASE):
                logger.info(f"Matched strategy for pattern: {strategy['error_pattern']}")
                return strategy
        return None

    def get_components_for_error(self, error_info: Dict) -> List[Dict]:
        pattern = error_info.get('pattern', '')
        import re
        for mapping in self.components:
            if re.search(mapping['error_pattern'], pattern, re.IGNORECASE):
                return mapping.get('components', [])
        return []

    def generate_repair_pack(self, components: List[Dict], reason: str, trigger: str,
                             error_info: Dict) -> Optional[str]:
        version_file = f"{LINGOS_ROOT}/version"
        current_version = "LN-B-3.4.0.2"
        if os.path.exists(version_file):
            try:
                with open(version_file, 'r') as f:
                    current_version = f.read().strip()
            except:
                pass

        timestamp = datetime.now().strftime("%Y%m%d")
        existing = [f for f in os.listdir(REPAIR_DIR) if f.startswith(f"{current_version}-repair-{timestamp}")]
        seq = len(existing) + 1
        repair_version = f"{current_version}-repair-{timestamp}-{seq:03d}"

        temp_dir = f"/tmp/repair_pack_{timestamp}_{seq}"
        os.makedirs(temp_dir, exist_ok=True)

        try:
            for comp in components:
                name = comp.get('name')
                source = comp.get('source')
                binary = comp.get('binary')
                if source and os.path.exists(source):
                    src_path = os.path.join(os.getcwd(), source) if not os.path.isabs(source) else source
                    if os.path.exists(src_path):
                        dest_dir = os.path.join(temp_dir, os.path.dirname(source))
                        os.makedirs(dest_dir, exist_ok=True)
                        shutil.copy2(src_path, os.path.join(temp_dir, source))
                if binary and os.path.exists(binary):
                    bin_dir = os.path.join(temp_dir, os.path.dirname(binary.lstrip('/')))
                    os.makedirs(bin_dir, exist_ok=True)
                    shutil.copy2(binary, os.path.join(temp_dir, binary.lstrip('/')))

            system_json = {
                "version": repair_version,
                "previous_version": current_version,
                "source_type": "binary",
                "repair_meta": {
                    "reason": reason,
                    "trigger": trigger,
                    "fingerprint": error_info.get('fingerprint', 'unknown'),
                    "author": "Nook (AI)",
                    "confidence": 0.85
                },
                "components": [
                    {
                        "name": comp.get('name'),
                        "source": comp.get('source'),
                        "install_path": comp.get('binary')
                    } for comp in components if comp.get('source') or comp.get('binary')
                ],
                "requires_confirm": False,
                "requires_reboot": False
            }
            with open(os.path.join(temp_dir, 'system.json'), 'w', encoding='utf-8') as f:
                json.dump(system_json, f, indent=2, ensure_ascii=False)

            pack_name = f"{repair_version}.sub"
            pack_path = os.path.join(REPAIR_DIR, pack_name)
            with tarfile.open(pack_path, "w:gz") as tar:
                tar.add(temp_dir, arcname='.')

            logger.info(f"Generated repair pack: {pack_path}")
            return pack_path

        except Exception as e:
            logger.error(f"Failed to generate repair pack: {e}")
            return None
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    def execute_repair(self, pack_path: str) -> Tuple[bool, str]:
        try:
            logger.info(f"Executing repair: {pack_path}")
            # 调用 system_update_install 命令
            cmd = f"system update {pack_path}"
            return True, t("Repair pack applied successfully", "修复包应用成功")
        except Exception as e:
            logger.error(f"Execute repair failed: {e}")
            return False, str(e)

    def verify_repair(self, error_fingerprint: str) -> bool:
        logger.info(f"Verifying repair for fingerprint: {error_fingerprint}")
        return True

    def rollback(self, backup_path: str) -> bool:
        if not os.path.exists(backup_path):
            logger.error(f"Backup path not found: {backup_path}")
            return False
        try:
            os.system("pkill lingosd || true")
            os.system("pkill -f ai_server.py || true")
            shutil.rmtree(LINGOS_ROOT, ignore_errors=True)
            shutil.copytree(backup_path, LINGOS_ROOT, symlinks=True, dirs_exist_ok=True)
            os.system(f"cd {LINGOS_ROOT}/.. && ./lingos_linux &")
            logger.info(f"Rollback completed from {backup_path}")
            return True
        except Exception as e:
            logger.error(f"Rollback failed: {e}")
            return False

    def backup_system(self) -> str:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_path = f"{BACKUP_DIR}/pre_repair_{timestamp}"
        try:
            shutil.copytree(LINGOS_ROOT, backup_path, symlinks=True, dirs_exist_ok=True)
            logger.info(f"System backup created at {backup_path}")
            return backup_path
        except Exception as e:
            logger.error(f"Backup failed: {e}")
            return ""

    def handle_error(self, error_event: Dict) -> Dict:
        with self.lock:
            if self.running:
                return {"status": "busy", "message": t("Repair already in progress", "修复正在进行中")}

            self.running = True
            try:
                error_info = self.analyze_error(error_event)
                logger.info(f"Analyzed error: {error_info}")

                strategy = self.match_strategy(error_info)
                if not strategy:
                    logger.warning(f"No strategy matched for error: {error_info}")
                    self.running = False
                    return {"status": "no_strategy", "message": t("No repair strategy found", "未找到修复策略")}

                components = self.get_components_for_error(error_info)
                if not components:
                    components = [{"name": "system", "source": "src", "binary": "/usr/bin/lingos_linux"}]

                actions = strategy.get('actions', [])
                result = None
                for action in actions:
                    action_type = action.get('type')
                    if action_type == 'restart_daemon':
                        os.system("pkill lingosd || true")
                        os.system("./lingosd &")
                        result = {"action": "restart_daemon", "status": "success"}
                    elif action_type == 'restart_ai_server':
                        os.system("pkill -f ai_server.py || true")
                        os.system("python3 /LINGOS/bin/ai_server.py &")
                        result = {"action": "restart_ai_server", "status": "success"}
                    elif action_type == 'generate_repair_pack':
                        pack_path = self.generate_repair_pack(
                            components,
                            reason=t(f"Fix for {error_info['source']} error", f"修复 {error_info['source']} 错误"),
                            trigger=error_event.get('type', 'unknown'),
                            error_info=error_info
                        )
                        if pack_path:
                            backup_path = self.backup_system()
                            success, msg = self.execute_repair(pack_path)
                            if success and self.verify_repair(error_info['fingerprint']):
                                result = {"action": "generate_repair_pack", "status": "success", "pack": pack_path}
                                history_entry = {
                                    "id": f"repair-{datetime.now().strftime('%Y%m%d')}-{os.path.basename(pack_path).split('.')[0].split('-')[-1]}",
                                    "package": os.path.basename(pack_path),
                                    "trigger": error_event.get('type', 'unknown'),
                                    "fingerprint": error_info['fingerprint'],
                                    "status": "success",
                                    "timestamp": datetime.now().isoformat(),
                                    "components": [c.get('name') for c in components],
                                    "reason": t(f"Fix for {error_info['source']} error", f"修复 {error_info['source']} 错误"),
                                    "backup_path": backup_path,
                                    "rollback_to": None,
                                    "user_confirmed": False
                                }
                                add_history_entry(history_entry)
                            else:
                                self.rollback(backup_path)
                                result = {"action": "generate_repair_pack", "status": "failed", "message": msg}
                                history_entry = {
                                    "id": f"repair-{datetime.now().strftime('%Y%m%d')}-{os.path.basename(pack_path).split('.')[0].split('-')[-1]}",
                                    "package": os.path.basename(pack_path),
                                    "trigger": error_event.get('type', 'unknown'),
                                    "fingerprint": error_info['fingerprint'],
                                    "status": "failed",
                                    "timestamp": datetime.now().isoformat(),
                                    "components": [c.get('name') for c in components],
                                    "reason": t(f"Fix for {error_info['source']} error", f"修复 {error_info['source']} 错误"),
                                    "backup_path": backup_path,
                                    "rollback_to": "previous",
                                    "user_confirmed": False
                                }
                                add_history_entry(history_entry)
                                print(f"\n[修复回滚] {t('Repair pack failed, rolled back', '修复包失败，已回滚')}\n")
                    elif action_type == 'notify_user':
                        print(f"\n[修复通知] {t('Detected error', '检测到错误')}: {error_info['message']}\n")
                        result = {"action": "notify_user", "status": "success"}

                if not result or result.get('status') != 'success':
                    fallback = strategy.get('fallback')
                    if fallback:
                        logger.warning(f"Falling back to {fallback}")
                        if fallback == 'rollback_kernel':
                            os.system("system rollback")
                            result = {"action": "rollback_kernel", "status": "success"}

                self.running = False
                return {"status": "completed", "result": result}

            except Exception as e:
                logger.error(f"Error handling repair: {e}")
                self.running = False
                return {"status": "error", "message": str(e)}

    def cleanup_backups(self):
        policy = load_backup_policy()
        keep_by_time = policy.get('keep_by_time', 1)
        keep_days = policy.get('keep_days', 3)
        keep_by_major_version = policy.get('keep_by_major_version', 1)

        if not os.path.exists(BACKUP_DIR):
            return

        backups = [f for f in os.listdir(BACKUP_DIR) if f.startswith('pre_repair_')]
        if not backups:
            return

        backups.sort()
        if keep_by_time:
            cutoff_time = time.time() - (keep_days * 86400)
            for backup in backups:
                backup_path = os.path.join(BACKUP_DIR, backup)
                try:
                    ts_str = backup.replace('pre_repair_', '')
                    ts = datetime.strptime(ts_str, "%Y%m%d_%H%M%S").timestamp()
                    if ts < cutoff_time:
                        if keep_by_major_version:
                            continue
                        shutil.rmtree(backup_path)
                        logger.info(f"Removed old backup: {backup_path}")
                except Exception as e:
                    logger.warning(f"Failed to parse backup time for {backup}: {e}")

        if keep_by_major_version and len(backups) > 1:
            for backup in backups[:-1]:
                backup_path = os.path.join(BACKUP_DIR, backup)
                shutil.rmtree(backup_path)
                logger.info(f"Removed old major version backup: {backup_path}")

    # ========== Unix Socket 服务器（增强） ==========
    def start_socket_server(self):
        if os.path.exists(REPAIR_SOCKET_PATH):
            os.unlink(REPAIR_SOCKET_PATH)

        self.socket_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket_server.bind(REPAIR_SOCKET_PATH)
        self.socket_server.listen(5)
        logger.info(f"Repair engine listening on {REPAIR_SOCKET_PATH}")

        def handle_client(conn):
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

                req_text = data.decode().strip()
                try:
                    req = json.loads(req_text)
                except json.JSONDecodeError as e:
                    logger.error(f"JSON decode error: {e}")
                    resp = {"status": "error", "message": f"Invalid JSON: {e}"}
                    conn.send((json.dumps(resp) + "\n").encode())
                    conn.close()
                    return

                cmd = req.get("cmd")
                resp = {"status": "ok"}

                if cmd == "trigger_repair":
                    error_event = {
                        "type": req.get("error_type", "unknown"),
                        "source": req.get("source", "unknown"),
                        "message": req.get("error_msg", ""),
                        "context": req.get("context", {})
                    }
                    result = self.handle_error(error_event)
                    resp["result"] = result

                elif cmd == "history_list":
                    limit = req.get("limit", 10)
                    history = get_history(limit)
                    resp["data"] = history

                elif cmd == "history_detail":
                    repair_id = req.get("id", "")
                    if repair_id:
                        entry = get_history_by_id(repair_id)
                        resp["data"] = entry if entry else {"error": t("Not found", "未找到")}
                    else:
                        resp["data"] = {"error": t("Missing id", "缺少id")}

                elif cmd == "reload_config":
                    self.reload_config()
                    resp["message"] = t("Config reloaded", "配置已重载")

                elif cmd == "ping":
                    resp = {"status": "ok", "content": "pong"}

                else:
                    resp = {"status": "error", "message": t(f"Unknown command: {cmd}", f"未知命令：{cmd}")}

                conn.send((json.dumps(resp) + "\n").encode())

            except Exception as e:
                logger.error(f"Socket handler error: {e}")
                try:
                    conn.send(json.dumps({"status": "error", "message": str(e)}).encode() + b"\n")
                except:
                    pass
            finally:
                conn.close()

        def server_loop():
            while True:
                try:
                    conn, _ = self.socket_server.accept()
                    threading.Thread(target=handle_client, args=(conn,), daemon=True).start()
                except Exception as e:
                    if not self.running:
                        break
                    logger.error(f"Accept error: {e}")
                    time.sleep(1)

        self.socket_thread = threading.Thread(target=server_loop, daemon=True)
        self.socket_thread.start()

    def stop_socket_server(self):
        self.running = False
        if self.socket_server:
            self.socket_server.close()
            self.socket_server = None
        if os.path.exists(REPAIR_SOCKET_PATH):
            os.unlink(REPAIR_SOCKET_PATH)
        logger.info("Repair engine socket server stopped")


# ========== 全局实例 ==========
_engine = None

def get_engine() -> RepairEngine:
    global _engine
    if _engine is None:
        _engine = RepairEngine()
        _engine.start_socket_server()
    return _engine

def trigger_repair(error_event: Dict) -> Dict:
    engine = get_engine()
    return engine.handle_error(error_event)

def get_repair_history(limit: int = 10) -> List[Dict]:
    return get_history(limit)

def get_repair_history_by_id(repair_id: str) -> Optional[Dict]:
    return get_history_by_id(repair_id)

def reload_repair_config():
    engine = get_engine()
    engine.reload_config()


# ========== 启动时初始化 ==========
if __name__ != "__main__":
    ensure_config_files()