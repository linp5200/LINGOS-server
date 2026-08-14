#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Diagnosis Engine
版本: LN-B-5.0.0.0-rc0.4
功能：AI 自主诊断引擎，分析日志、推断根因、匹配修复策略

使用方式：
    from diagnosis_engine import DiagnosisEngine
    engine = DiagnosisEngine()
    result = engine.analyze(error_signature)
    repair_action = engine.match_repair_strategy(diagnosis)
"""

import os
import re
import json
import logging
from typing import Dict, Any, List, Optional, Tuple
from datetime import datetime, timedelta
import subprocess

logger = logging.getLogger("DiagnosisEngine")

# ========== 配置文件路径 ==========
REPAIR_STRATEGIES_PATH = "/LINGOS/system/config/repair_strategies.json"
PREFERENCES_PATH = "/LINGOS/system/config/repair_preferences.conf"
LOG_DIR = "/LINGOS/Debug"
REPAIR_HISTORY_PATH = "/LINGOS/state/repair_history.json"


class DiagnosisResult:
    """诊断结果"""
    def __init__(self):
        self.error_signature: str = ""
        self.root_cause: str = ""
        self.confidence: float = 0.0
        self.suggestions: List[str] = []
        self.repair_action: str = ""
        self.repair_tier: int = 1  # 1-3
        self.detected_at: str = ""


class DiagnosisEngine:
    """AI 自主诊断引擎"""
    
    def __init__(self):
        self.strategies = self._load_strategies()
        self.preferences = self._load_preferences()
        self.history = self._load_history()
        self._error_patterns = self._compile_patterns()
    
    def _load_strategies(self) -> Dict:
        """加载修复策略"""
        try:
            if os.path.exists(REPAIR_STRATEGIES_PATH):
                with open(REPAIR_STRATEGIES_PATH, 'r') as f:
                    return json.load(f)
        except Exception as e:
            logger.warning(f"Failed to load repair strategies: {e}")
        return {"strategies": []}
    
    def _load_preferences(self) -> Dict:
        """加载用户偏好"""
        prefs = {
            "allow_auto_repair": True,
            "allow_auto_cleanup": True,
            "allow_auto_restart": False,
            "allow_auto_rollback": False,
            "require_confirm": True,
            "verify_after_repair": True
        }
        try:
            if os.path.exists(PREFERENCES_PATH):
                with open(PREFERENCES_PATH, 'r') as f:
                    for line in f:
                        if line.startswith('#') or not line.strip():
                            continue
                        if '=' in line:
                            key, val = line.split('=', 1)
                            key = key.strip()
                            val = val.strip().lower()
                            if key in prefs:
                                if val in ('true', 'false'):
                                    prefs[key] = (val == 'true')
                                elif val.isdigit():
                                    prefs[key] = int(val)
        except Exception as e:
            logger.warning(f"Failed to load repair preferences: {e}")
        return prefs
    
    def _load_history(self) -> List[Dict]:
        """加载修复历史"""
        try:
            if os.path.exists(REPAIR_HISTORY_PATH):
                with open(REPAIR_HISTORY_PATH, 'r') as f:
                    return json.load(f)
        except Exception:
            pass
        return []
    
    def _compile_patterns(self) -> List[Tuple[re.Pattern, str, str]]:
        """编译错误模式"""
        patterns = []
        error_patterns = [
            (r"(?i)connection refused", "connection_refused", "Service port is not responding"),
            (r"(?i)timeout|timed out", "timeout", "Operation took too long to complete"),
            (r"(?i)permission denied", "permission_denied", "Insufficient permissions"),
            (r"(?i)no such file|not found", "file_not_found", "Required file is missing"),
            (r"(?i)disk full|no space", "disk_full", "Storage is full"),
            (r"(?i)memory.*full|out of memory", "out_of_memory", "System memory exhausted"),
            (r"(?i)segmentation fault", "segfault", "Program crashed with segfault"),
            (r"(?i)ModuleNotFoundError|ImportError", "module_missing", "Python module is missing"),
            (r"(?i)401|unauthorized", "auth_failed", "Authentication failed"),
            (r"(?i)429|rate limit", "rate_limited", "Rate limit exceeded"),
            (r"(?i)500|internal server error", "server_error", "Server internal error"),
            (r"(?i)socket.*fail|bind.*fail", "socket_error", "Socket operation failed"),
            (r"(?i)corrupt|broken|invalid json", "corrupt_data", "Data corruption detected"),
        ]
        for pattern, error_type, description in error_patterns:
            patterns.append((re.compile(pattern), error_type, description))
        return patterns
    
    def _scan_logs(self, since_minutes: int = 10) -> List[str]:
        """扫描日志文件"""
        errors = []
        cutoff = datetime.now() - timedelta(minutes=since_minutes)
        
        if not os.path.exists(LOG_DIR):
            return errors
        
        for filename in os.listdir(LOG_DIR):
            if filename.endswith('.log'):
                filepath = os.path.join(LOG_DIR, filename)
                try:
                    mtime = datetime.fromtimestamp(os.path.getmtime(filepath))
                    if mtime < cutoff:
                        continue
                    with open(filepath, 'r') as f:
                        for line in f:
                            if 'ERROR' in line or 'FATAL' in line or 'FAIL' in line:
                                errors.append(line.strip())
                except Exception:
                    continue
        
        return errors
    
    def _match_error(self, error_msg: str) -> Tuple[str, str, float]:
        """匹配错误模式"""
        for pattern, error_type, description in self._error_patterns:
            if pattern.search(error_msg):
                # 置信度：如果完全匹配，提高置信度
                if pattern.match(error_msg):
                    return error_type, description, 0.9
                return error_type, description, 0.7
        return "unknown", "Unknown error", 0.3
    
    def _determine_repair_tier(self, error_type: str, confidence: float) -> int:
        """确定修复等级"""
        if confidence < 0.4:
            return 3  # 低置信度 → 需要用户确认
        if error_type in ("segfault", "disk_full", "out_of_memory"):
            return 2  # 严重但可自动修复
        if error_type in ("file_not_found", "module_missing", "connection_refused"):
            return 1  # 常见错误，可自动修复
        return 2
    
    def analyze(self, error_signature: str, context: str = "") -> DiagnosisResult:
        """分析错误，生成诊断结果"""
        result = DiagnosisResult()
        result.error_signature = error_signature
        result.detected_at = datetime.now().isoformat()
        
        # 1. 匹配错误模式
        error_type, description, confidence = self._match_error(error_signature)
        result.confidence = confidence
        
        # 2. 扫描日志获取上下文
        if not context:
            logs = self._scan_logs(5)
            if logs:
                context = "\n".join(logs[:5])
        
        # 3. 生成诊断
        diagnosis_map = {
            "connection_refused": "Service is not responding. Check if the service is running and the port is correct.",
            "timeout": "Operation took too long. Check network connectivity and system load.",
            "permission_denied": "Insufficient permissions. Check file ownership and access rights.",
            "file_not_found": "Required file is missing. Check if the file was deleted or moved.",
            "disk_full": "Storage is full. Clear unnecessary files or expand storage.",
            "out_of_memory": "System memory exhausted. Close unnecessary processes or increase memory.",
            "segfault": "Program crashed due to memory access violation. Check for bugs or corruption.",
            "module_missing": "Python module is missing. Install the required module.",
            "auth_failed": "Authentication failed. Check API key or credentials.",
            "rate_limited": "Rate limit exceeded. Reduce request frequency.",
            "server_error": "Server internal error. Check server logs and retry later.",
            "socket_error": "Socket operation failed. Check network configuration.",
            "corrupt_data": "Data corruption detected. Restore from backup.",
            "unknown": "Unknown error. Check logs for details."
        }
        result.root_cause = diagnosis_map.get(error_type, "Unable to determine root cause.")
        
        # 4. 生成建议
        suggestions_map = {
            "connection_refused": ["Check if the service is running: systemctl status <service>", "Check the port configuration"],
            "timeout": ["Check network connectivity: ping <host>", "Increase timeout in configuration"],
            "permission_denied": ["Check file permissions: ls -l <path>", "Use sudo or change owner"],
            "file_not_found": ["Check if file exists: ls -la <path>", "Restore from backup"],
            "disk_full": ["Clear cache and old logs", "Check large files: du -sh * | sort -hr | head"],
            "out_of_memory": ["Check memory usage: free -h", "Close unnecessary processes"],
            "segfault": ["Check core dump: /LINGOS/Dump/", "Restart the service"],
            "module_missing": ["Install module: pip install <module>", "Check Python environment"],
            "auth_failed": ["Check API key: cat /LINGOS/system/config/ai_config.json", "Re-run configuration wizard"],
            "rate_limited": ["Wait and retry", "Reduce request frequency"],
            "server_error": ["Check server logs", "Retry later"],
            "socket_error": ["Check network configuration", "Check firewall rules"],
            "corrupt_data": ["Restore from backup", "Run system check"],
            "unknown": ["Check logs: tail -f /LINGOS/Debug/lingos_*.log", "Run system debug info"]
        }
        result.suggestions = suggestions_map.get(error_type, ["Check logs for more details."])
        
        # 5. 确定修复等级
        result.repair_tier = self._determine_repair_tier(error_type, confidence)
        
        # 6. 匹配修复策略
        result.repair_action = self.match_repair_strategy(result)
        
        return result
    
    def match_repair_strategy(self, diagnosis: DiagnosisResult) -> str:
        """匹配修复策略"""
        strategies = self.strategies.get("strategies", [])
        error_type, _, _ = self._match_error(diagnosis.error_signature)
        
        # 检查用户偏好
        if not self.preferences.get("allow_auto_repair", True):
            return "notify_user"
        
        # 根据修复等级决定
        if diagnosis.repair_tier == 1:
            # 自动修复
            if diagnosis.root_cause and "missing" in diagnosis.root_cause.lower():
                return "install_missing"
            if "disk" in diagnosis.root_cause.lower() or "full" in diagnosis.root_cause.lower():
                return "clean_cache"
            return "restart_service"
        elif diagnosis.repair_tier == 2:
            # 需要通知用户
            return "notify_user"
        else:
            # 需要用户确认
            return "require_confirm"
        
        # 策略匹配
        for strategy in strategies:
            patterns = strategy.get("patterns", [])
            action = strategy.get("action", "notify_user")
            for pattern in patterns:
                if re.search(pattern, diagnosis.error_signature, re.IGNORECASE):
                    # 检查是否需要用户确认
                    if strategy.get("require_confirm", False):
                        return "require_confirm"
                    return action
        
        return "notify_user"
    
    def repair(self, action: str, diagnosis: DiagnosisResult) -> Tuple[bool, str]:
        """执行修复动作"""
        logger.info(f"Executing repair action: {action}")
        
        result_msg = ""
        success = False
        
        # 检查用户偏好
        if action == "restart_service" and not self.preferences.get("allow_auto_restart", False):
            return False, "Auto-restart disabled by user preference"
        
        if action.startswith("rollback") and not self.preferences.get("allow_auto_rollback", False):
            return False, "Auto-rollback disabled by user preference"
        
        # 执行修复
        if action == "clean_cache":
            # 清理缓存
            cache_dir = "/LINGOS/cache"
            try:
                subprocess.run(f"rm -rf {cache_dir}/* 2>/dev/null", shell=True, timeout=10)
                success = True
                result_msg = "Cache cleaned successfully"
            except Exception as e:
                result_msg = f"Failed to clean cache: {e}"
        
        elif action == "restart_service":
            # 重启服务
            services = ["lingosd", "ai_server.py"]
            for svc in services:
                try:
                    subprocess.run(f"pkill -f {svc}; sleep 1", shell=True, timeout=5)
                    if svc == "lingosd":
                        subprocess.run(f"./lingosd &", shell=True, timeout=5)
                    else:
                        subprocess.run(f"python3 /LINGOS/bin/{svc} &", shell=True, timeout=5)
                    success = True
                    result_msg = f"Service {svc} restarted"
                except Exception as e:
                    result_msg = f"Failed to restart {svc}: {e}"
                    success = False
        
        elif action == "install_missing":
            # 安装缺失模块（简化）
            try:
                subprocess.run("pip3 install --break-system-packages tiktoken sentence-transformers 2>/dev/null", shell=True, timeout=60)
                success = True
                result_msg = "Missing modules installed successfully"
            except Exception as e:
                result_msg = f"Failed to install modules: {e}"
                success = False
        
        elif action == "notify_user":
            result_msg = "User notification: " + diagnosis.root_cause
            success = True
        
        elif action == "require_confirm":
            result_msg = "Action requires user confirmation: " + diagnosis.root_cause
            success = False
        
        else:
            result_msg = f"Unknown repair action: {action}"
            success = False
        
        # 记录历史
        self._record_repair(diagnosis, action, success)
        
        return success, result_msg
    
    def _record_repair(self, diagnosis: DiagnosisResult, action: str, success: bool):
        """记录修复历史"""
        entry = {
            "timestamp": datetime.now().isoformat(),
            "error_signature": diagnosis.error_signature,
            "root_cause": diagnosis.root_cause,
            "repair_action": action,
            "success": success,
            "confidence": diagnosis.confidence
        }
        self.history.append(entry)
        # 只保留最近 100 条
        if len(self.history) > 100:
            self.history = self.history[-100:]
        
        try:
            os.makedirs(os.path.dirname(REPAIR_HISTORY_PATH), exist_ok=True)
            with open(REPAIR_HISTORY_PATH, 'w') as f:
                json.dump(self.history, f, indent=2)
        except Exception as e:
            logger.warning(f"Failed to save repair history: {e}")
    
    def get_history(self, limit: int = 20) -> List[Dict]:
        """获取修复历史"""
        return self.history[-limit:]
    
    def get_stats(self) -> Dict:
        """获取统计信息"""
        total = len(self.history)
        success_count = sum(1 for h in self.history if h.get("success"))
        return {
            "total_repairs": total,
            "successful": success_count,
            "failed": total - success_count,
            "success_rate": success_count / total if total > 0 else 0
        }


# ========== 便捷函数 ==========
_engine = None

def get_engine() -> DiagnosisEngine:
    """获取诊断引擎单例"""
    global _engine
    if _engine is None:
        _engine = DiagnosisEngine()
    return _engine

def analyze_error(error_signature: str, context: str = "") -> DiagnosisResult:
    """分析错误"""
    return get_engine().analyze(error_signature, context)

def execute_repair(action: str, diagnosis: DiagnosisResult) -> Tuple[bool, str]:
    """执行修复"""
    return get_engine().repair(action, diagnosis)