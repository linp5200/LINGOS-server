#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Plugin Loader
版本: LN-B-4.2.0.0
功能：加载、管理和执行 Python 插件
"""

import os
import sys
import importlib
import logging
import threading
from typing import List, Dict, Optional, Any, Tuple
from pathlib import Path

# ========== 导入子模块 ==========
from plugin_discovery import discover_plugins, ensure_plugin_dirs
from lingos_plugin import Plugin, set_language

# ========== 日志配置 ==========
logger = logging.getLogger("PluginLoader")

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en


# =============================================================
# 插件加载器类
# =============================================================

class PluginLoader:
    """Python 插件加载器（单例）"""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super(PluginLoader, cls).__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True

        self._plugins: Dict[str, Plugin] = {}  # name -> plugin instance
        self._skills: Dict[str, Dict] = {}    # skill_name -> {plugin, func, risk}
        self._commands: Dict[str, Dict] = {}  # cmd_name -> {plugin, func}
        self._loaded = False
        self._lock = threading.RLock()

        logger.info("PluginLoader initialized")

    def load_all(self, language: str = "en") -> int:
        """
        加载所有插件
        Args:
            language: 语言偏好
        Returns:
            成功加载的插件数量
        """
        global _current_lang
        _current_lang = language
        set_language(language)

        with self._lock:
            if self._loaded:
                logger.warning("load_all: already loaded")
                return len(self._plugins)

            logger.info("load_all: starting plugin discovery")

            # 确保插件目录存在
            ensure_plugin_dirs()

            # 发现插件
            plugin_infos = discover_plugins()
            loaded_count = 0

            for info in plugin_infos:
                if info.get("error"):
                    logger.warning(f"load_all: {info['name']} has error: {info['error']}")
                    continue

                if info.get("class") is None:
                    logger.warning(f"load_all: {info['name']} has no plugin class")
                    continue

                try:
                    # 实例化插件
                    plugin_class = info["class"]
                    plugin_instance = plugin_class()

                    # 验证插件
                    if not isinstance(plugin_instance, Plugin):
                        logger.warning(f"load_all: {info['name']} is not a Plugin instance")
                        continue

                    if not plugin_instance.name:
                        plugin_instance.name = info['name']

                    # 调用 on_load
                    if not plugin_instance.on_load():
                        logger.warning(f"load_all: {plugin_instance.name} on_load returned False")
                        continue

                    plugin_instance._loaded = True

                    # 注册技能
                    skills = plugin_instance.get_skills()
                    for skill_name, skill_info in skills.items():
                        self._skills[skill_name] = {
                            "plugin": plugin_instance,
                            "func": skill_info["func"],
                            "risk": skill_info.get("risk", "low"),
                            "description": skill_info.get("description", ""),
                            "need_confirm": skill_info.get("need_confirm", False)
                        }
                        logger.debug(f"load_all: registered skill '{skill_name}' from {plugin_instance.name}")

                    # 注册命令
                    commands = plugin_instance.get_commands()
                    for cmd_name, cmd_info in commands.items():
                        self._commands[cmd_name] = {
                            "plugin": plugin_instance,
                            "func": cmd_info["func"],
                            "description": cmd_info.get("description", "")
                        }
                        logger.debug(f"load_all: registered command '{cmd_name}' from {plugin_instance.name}")

                    # 保存插件
                    self._plugins[plugin_instance.name] = plugin_instance
                    loaded_count += 1
                    logger.info(f"load_all: loaded plugin '{plugin_instance.name}' v{plugin_instance.version}")

                except Exception as e:
                    logger.error(f"load_all: error loading {info['name']}: {e}")

            self._loaded = True
            logger.info(f"load_all: loaded {loaded_count} plugins, {len(self._skills)} skills, {len(self._commands)} commands")
            return loaded_count

    def load_plugin(self, file_path: str) -> Optional[Plugin]:
        """
        加载单个插件文件（热加载）
        Args:
            file_path: 插件文件路径
        Returns:
            加载的插件实例，或 None
        """
        with self._lock:
            logger.info(f"load_plugin: loading {file_path}")

            try:
                # 获取模块名
                module_name = os.path.basename(file_path)[:-3]

                # 发现插件
                from plugin_discovery import load_plugin_module
                info = load_plugin_module(file_path, module_name)

                if info is None or info.get("class") is None:
                    logger.error(f"load_plugin: invalid plugin: {file_path}")
                    return None

                # 实例化插件
                plugin_class = info["class"]
                plugin_instance = plugin_class()

                if not isinstance(plugin_instance, Plugin):
                    logger.error(f"load_plugin: {file_path} is not a Plugin")
                    return None

                # 检查是否已加载
                if plugin_instance.name in self._plugins:
                    logger.warning(f"load_plugin: plugin {plugin_instance.name} already loaded")
                    return None

                # 调用 on_load
                if not plugin_instance.on_load():
                    logger.warning(f"load_plugin: {plugin_instance.name} on_load failed")
                    return None

                plugin_instance._loaded = True

                # 注册技能
                skills = plugin_instance.get_skills()
                for skill_name, skill_info in skills.items():
                    self._skills[skill_name] = {
                        "plugin": plugin_instance,
                        "func": skill_info["func"],
                        "risk": skill_info.get("risk", "low"),
                        "description": skill_info.get("description", ""),
                        "need_confirm": skill_info.get("need_confirm", False)
                    }

                # 注册命令
                commands = plugin_instance.get_commands()
                for cmd_name, cmd_info in commands.items():
                    self._commands[cmd_name] = {
                        "plugin": plugin_instance,
                        "func": cmd_info["func"],
                        "description": cmd_info.get("description", "")
                    }

                self._plugins[plugin_instance.name] = plugin_instance
                logger.info(f"load_plugin: loaded {plugin_instance.name}")
                return plugin_instance

            except Exception as e:
                logger.error(f"load_plugin: error loading {file_path}: {e}")
                return None

    def unload_plugin(self, name: str) -> bool:
        """
        卸载插件
        Args:
            name: 插件名称
        Returns:
            True 成功，False 失败
        """
        with self._lock:
            if name not in self._plugins:
                logger.warning(f"unload_plugin: plugin {name} not found")
                return False

            plugin = self._plugins[name]

            # 调用 on_stop
            if hasattr(plugin, 'on_stop'):
                plugin.on_stop()

            # 移除技能
            to_remove = []
            for skill_name, skill_info in self._skills.items():
                if skill_info["plugin"] == plugin:
                    to_remove.append(skill_name)
            for skill_name in to_remove:
                del self._skills[skill_name]
                logger.debug(f"unload_plugin: removed skill {skill_name}")

            # 移除命令
            to_remove = []
            for cmd_name, cmd_info in self._commands.items():
                if cmd_info["plugin"] == plugin:
                    to_remove.append(cmd_name)
            for cmd_name in to_remove:
                del self._commands[cmd_name]
                logger.debug(f"unload_plugin: removed command {cmd_name}")

            # 调用 on_unload
            if hasattr(plugin, 'on_unload'):
                plugin.on_unload()

            # 从字典移除
            del self._plugins[name]
            logger.info(f"unload_plugin: unloaded {name}")
            return True

    def get_plugin(self, name: str) -> Optional[Plugin]:
        """获取插件实例"""
        return self._plugins.get(name)

    def list_plugins(self) -> List[Dict]:
        """列出所有已加载插件"""
        result = []
        for name, plugin in self._plugins.items():
            info = plugin.get_info()
            info["skill_count"] = sum(1 for s in self._skills.values() if s["plugin"] == plugin)
            info["command_count"] = sum(1 for c in self._commands.values() if c["plugin"] == plugin)
            result.append(info)
        return result

    def get_all_skills(self) -> Dict[str, Dict]:
        """获取所有已注册技能"""
        return self._skills.copy()

    def get_all_commands(self) -> Dict[str, Dict]:
        """获取所有已注册命令"""
        return self._commands.copy()

    def execute_skill(self, skill_name: str, args_json: str) -> Tuple[bool, str]:
        """
        执行技能
        Args:
            skill_name: 技能名称
            args_json: JSON 参数字符串
        Returns:
            (成功标志, 结果字符串)
        """
        if skill_name not in self._skills:
            return False, f"Skill '{skill_name}' not found"

        skill_info = self._skills[skill_name]
        try:
            result = skill_info["func"](args_json)
            if isinstance(result, dict):
                return True, json.dumps(result, ensure_ascii=False)
            return True, str(result)
        except Exception as e:
            logger.error(f"execute_skill: error executing {skill_name}: {e}")
            return False, str(e)

    def execute_command(self, cmd_name: str, args: str) -> str:
        """
        执行命令
        Args:
            cmd_name: 命令名称
            args: 参数字符串
        Returns:
            命令输出
        """
        if cmd_name not in self._commands:
            return f"Command '{cmd_name}' not found"

        cmd_info = self._commands[cmd_name]
        try:
            result = cmd_info["func"](args)
            return str(result) if result is not None else ""
        except Exception as e:
            logger.error(f"execute_command: error executing {cmd_name}: {e}")
            return f"Error: {e}"

    def is_loaded(self) -> bool:
        """检查是否已加载"""
        return self._loaded


# =============================================================
# 全局单例访问
# =============================================================

_loader = None

def get_loader() -> PluginLoader:
    """获取全局插件加载器实例"""
    global _loader
    if _loader is None:
        _loader = PluginLoader()
    return _loader


def load_plugins(language: str = "en") -> int:
    """加载所有插件（便捷函数）"""
    return get_loader().load_all(language)


def get_plugin_skills() -> Dict[str, Dict]:
    """获取所有技能（便捷函数）"""
    return get_loader().get_all_skills()


def get_plugin_commands() -> Dict[str, Dict]:
    """获取所有命令（便捷函数）"""
    return get_loader().get_all_commands()


# =============================================================
# 模块自检
# =============================================================

if __name__ == "__main__":
    # 简单测试
    print("LING OS Plugin Loader Test")
    loader = get_loader()
    count = loader.load_all()
    print(f"Loaded {count} plugins")
    print(f"Skills: {loader.get_all_skills().keys()}")
    print(f"Commands: {loader.get_all_commands().keys()}")