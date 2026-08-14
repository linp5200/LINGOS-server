#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Config Helpers
版本: LN-B-3.4.0.2
功能：配置文件读写、API Key验证、模型列表获取
"""

import os
import sys
import json
import requests
import logging
from typing import Dict, Any, Optional, List, Tuple

# ========== 日志配置 ==========
logger = logging.getLogger("ConfigHelpers")

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def load_config_language():
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

load_config_language()

# ========== 常量 ==========
CONFIG_PATH = "/LINGOS/system/config/ai_config.json"
DEEPSEEK_BASE_URL = "https://api.deepseek.com"

# ========== 配置文件读写 ==========

def read_json_config(path: str) -> Optional[Dict]:
    """读取JSON配置文件"""
    try:
        if not os.path.exists(path):
            logger.warning(f"Config file not found: {path}")
            return None
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        logger.error(f"Invalid JSON in {path}: {e}")
        return None
    except Exception as e:
        logger.error(f"Failed to read {path}: {e}")
        return None

def write_json_config(path: str, data: Dict) -> bool:
    """写入JSON配置文件"""
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        logger.info(f"Written config to {path}")
        return True
    except Exception as e:
        logger.error(f"Failed to write {path}: {e}")
        return False

def read_ai_config() -> Optional[Dict]:
    """读取AI配置"""
    return read_json_config(CONFIG_PATH)

def write_ai_config(data: Dict) -> bool:
    """写入AI配置"""
    return write_json_config(CONFIG_PATH, data)

def get_config_value(key: str, default: Any = None) -> Any:
    """获取配置值"""
    config = read_ai_config()
    if config is None:
        return default
    keys = key.split(".")
    value = config
    for k in keys:
        if isinstance(value, dict) and k in value:
            value = value[k]
        else:
            return default
    return value

def set_config_value(key: str, value: Any) -> bool:
    """设置配置值"""
    config = read_ai_config()
    if config is None:
        config = {}
    keys = key.split(".")
    target = config
    for k in keys[:-1]:
        if k not in target or not isinstance(target[k], dict):
            target[k] = {}
        target = target[k]
    target[keys[-1]] = value
    return write_ai_config(config)

# ========== API Key 验证 ==========

def validate_deepseek_api_key(api_key: str, base_url: str = DEEPSEEK_BASE_URL) -> Tuple[bool, str]:
    """
    验证DeepSeek API Key是否有效
    返回: (是否有效, 消息)
    """
    if not api_key or not api_key.strip():
        return False, t("API Key is empty", "API Key 为空")
    
    if not api_key.startswith("sk-"):
        return False, t("Invalid API Key format (should start with 'sk-')", "API Key 格式无效（应以 'sk-' 开头）")
    
    try:
        url = f"{base_url}/models"
        headers = {"Authorization": f"Bearer {api_key}"}
        resp = requests.get(url, headers=headers, timeout=10)
        
        if resp.status_code == 200:
            return True, t("API Key is valid", "API Key 有效")
        elif resp.status_code == 401:
            return False, t("API Key is invalid or expired", "API Key 无效或已过期")
        else:
            return False, t(f"Server returned {resp.status_code}", f"服务器返回 {resp.status_code}")
    except requests.exceptions.Timeout:
        return False, t("Connection timeout", "连接超时")
    except requests.exceptions.ConnectionError:
        return False, t("Cannot connect to DeepSeek server", "无法连接到 DeepSeek 服务器")
    except Exception as e:
        return False, str(e)

def validate_ollama_connection(url: str) -> Tuple[bool, str]:
    """
    验证Ollama连接是否正常
    返回: (是否正常, 消息)
    """
    if not url or not url.strip():
        return False, t("Ollama URL is empty", "Ollama URL 为空")
    
    try:
        resp = requests.get(f"{url}/api/tags", timeout=5)
        if resp.status_code == 200:
            return True, t("Ollama is running", "Ollama 运行正常")
        else:
            return False, t(f"Ollama returned {resp.status_code}", f"Ollama 返回 {resp.status_code}")
    except requests.exceptions.Timeout:
        return False, t("Ollama connection timeout", "Ollama 连接超时")
    except requests.exceptions.ConnectionError:
        return False, t("Cannot connect to Ollama", "无法连接到 Ollama")
    except Exception as e:
        return False, str(e)

# ========== 模型列表获取 ==========

def get_ollama_models(url: str) -> List[str]:
    """
    获取Ollama已安装的模型列表
    """
    try:
        resp = requests.get(f"{url}/api/tags", timeout=10)
        if resp.status_code == 200:
            data = resp.json()
            models = data.get("models", [])
            return [m.get("name", "unknown") for m in models]
        else:
            logger.warning(f"Failed to get Ollama models: {resp.status_code}")
            return []
    except Exception as e:
        logger.warning(f"Failed to get Ollama models: {e}")
        return []

def get_deepseek_models(api_key: str, base_url: str = DEEPSEEK_BASE_URL) -> List[str]:
    """
    获取DeepSeek可用模型列表（需要有效API Key）
    """
    if not api_key or not api_key.strip():
        return []
    
    try:
        url = f"{base_url}/models"
        headers = {"Authorization": f"Bearer {api_key}"}
        resp = requests.get(url, headers=headers, timeout=10)
        if resp.status_code == 200:
            data = resp.json()
            models = data.get("data", [])
            return [m.get("id", "unknown") for m in models]
        else:
            logger.warning(f"Failed to get DeepSeek models: {resp.status_code}")
            return []
    except Exception as e:
        logger.warning(f"Failed to get DeepSeek models: {e}")
        return []

# ========== 默认配置生成 ==========

def generate_default_ai_config(lang: str = "en") -> Dict:
    """生成默认AI配置"""
    is_zh = lang == "zh"
    return {
        "backend": "ollama",
        "language": lang,
        "thinking_enabled": True,
        "stream_enabled": True,
        "show_thinking": True,
        "stream_style": "color",
        "max_context_tokens": 32768,
        "truncation_strategy": "sliding",
        "meta_info_enabled": True,
        "socket_timeout": 300,
        "user_id": "LING-AIO-NOOK",
        "ollama": {
            "url": "http://127.0.0.1:8080",
            "model": "glm-4.6:cloud"
        },
        "deepseek": {
            "api_key": "",
            "model": "deepseek-v4-pro",
            "base_url": "https://api.deepseek.com",
            "reasoning_effort": "high",
            "enable_tools": True,
            "parallel_tools": True
        },
        "sub_ai": {
            "api_key": "",
            "model": "deepseek-v4-pro",
            "base_url": "https://api.deepseek.com"
        }
    }

def generate_default_user_profile(lang: str = "en") -> Dict:
    """生成默认用户配置"""
    name = "先生" if lang == "zh" else "Sir"
    return {
        "user_name": name
    }

def generate_default_language_map(lang: str = "en") -> Dict:
    """生成默认语言映射"""
    lang_code = "zh-CN" if lang == "zh" else "en-US"
    return {
        "CN": lang_code,
        "DEFAULT": "en-US"
    }

def generate_default_risk_policy() -> Dict:
    """生成默认风险策略"""
    return {
        "risk_levels": {
            "low": {
                "description": "Low risk operations, execute directly",
                "require_confirm": False,
                "auto_allow_subai": True
            },
            "medium": {
                "description": "Medium risk operations, execute directly",
                "require_confirm": False,
                "auto_allow_subai": True
            },
            "high": {
                "description": "High risk operations, require user confirmation",
                "require_confirm": True,
                "auto_allow_subai": False
            },
            "critical": {
                "description": "Critical risk operations, require second confirmation",
                "require_confirm": True,
                "auto_allow_subai": False,
                "second_confirm": True
            }
        },
        "skill_defaults": {
            "file_write": "medium",
            "file_delete": "medium",
            "file_copy": "medium",
            "file_move": "medium",
            "script_create": "medium",
            "script_exec": "high",
            "process_kill": "high",
            "package_install": "critical",
            "package_remove": "critical",
            "service_restart": "high",
            "config_write": "critical",
            "sys_command": "critical",
            "user_add": "critical",
            "cron_add": "high",
            "system_reboot": "critical",
            "system_update": "critical",
            "defense_mode": "medium",
            "perm_set": "medium"
        }
    }

# ========== 初始化配置 ==========

def ensure_configs_exist(lang: str = "en") -> bool:
    """确保所有必需配置文件存在，若不存在则创建默认"""
    ai_config = read_ai_config()
    if ai_config is None:
        ai_config = generate_default_ai_config(lang)
        if not write_ai_config(ai_config):
            return False
    
    # 确保 user_profile.json 存在
    profile_path = "/LINGOS/system/config/user_profile.json"
    if not os.path.exists(profile_path):
        profile = generate_default_user_profile(lang)
        write_json_config(profile_path, profile)
    
    # 确保 language_map.json 存在
    lang_map_path = "/LINGOS/system/config/language_map.json"
    if not os.path.exists(lang_map_path):
        lang_map = generate_default_language_map(lang)
        write_json_config(lang_map_path, lang_map)
    
    # 确保 risk_policy.json 存在
    risk_path = "/LINGOS/system/config/risk_policy.json"
    if not os.path.exists(risk_path):
        risk_policy = generate_default_risk_policy()
        write_json_config(risk_path, risk_policy)
    
    # 确保 state.json 存在
    state_path = "/LINGOS/Ensystem/state.json"
    if not os.path.exists(state_path):
        state = {
            "system_configured": False,
            "last_config_time": "",
            "mode": "app"
        }
        write_json_config(state_path, state)
    
    return True

# ========== 语言设置 ==========

def set_language(lang: str):
    """设置语言偏好"""
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang
        # 更新配置文件
        config = read_ai_config()
        if config is not None:
            config["language"] = lang
            write_ai_config(config)
        logger.info(f"Language set to: {lang}")
        return True
    return False

def get_language() -> str:
    return _current_lang


# ========== 自测 ==========
if __name__ == "__main__":
    print(t("Testing Config Helpers", "测试配置助手"))
    
    # 测试配置读写
    config = read_ai_config()
    if config:
        print(t("AI Config loaded successfully", "AI配置加载成功"))
    else:
        print(t("AI Config not found, generating default", "AI配置未找到，生成默认配置"))
        ensure_configs_exist("en")
        config = read_ai_config()
        print(json.dumps(config, indent=2, ensure_ascii=False))
    
    # 测试API Key验证
    print("\n" + t("Testing API Key validation", "测试API Key验证"))
    valid, msg = validate_deepseek_api_key("")
    print(f"{t('Empty API Key', '空API Key')}: {msg}")
    
    # 测试模型列表
    print("\n" + t("Testing model list", "测试模型列表"))
    models = get_ollama_models("http://127.0.0.1:8080")
    print(f"{t('Ollama models', 'Ollama模型')}: {models if models else t('None or not connected', '无或未连接')}")
    
    print(t("\nTest complete", "\n测试完成"))