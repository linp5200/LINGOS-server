#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Python Plugin Base
版本: LN-B-4.2.0.0
功能：插件基类、@Skill 和 @Command 装饰器
"""

import os
import sys
import json
import inspect
import logging
from typing import Dict, Any, Callable, Optional, List, Tuple
from functools import wraps

# ========== 日志配置 ==========
logger = logging.getLogger("LINGOSPlugin")

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def set_language(lang: str):
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang


# =============================================================
# 装饰器：注册技能
# =============================================================

class Skill:
    """
    技能装饰器，用于将函数注册为 AI 技能

    用法:
        @Skill(name="my_skill", risk="low", description="My custom skill")
        def my_skill(args):
            return {"result": "success"}
    """
    def __init__(self, name: str, risk: str = "low", description: str = "", need_confirm: bool = False):
        self.name = name
        self.risk = risk
        self.description = description
        self.need_confirm = need_confirm
        self.func = None

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            return func(*args, **kwargs)
        wrapper._skill_name = self.name
        wrapper._skill_risk = self.risk
        wrapper._skill_description = self.description
        wrapper._skill_need_confirm = self.need_confirm
        wrapper._is_skill = True
        return wrapper


# =============================================================
# 装饰器：注册 Shell 命令
# =============================================================

class Command:
    """
    命令装饰器，用于将函数注册为 Shell 命令

    用法:
        @Command(name="mycmd", description="My custom command")
        def my_cmd(args):
            return "Hello from my command"
    """
    def __init__(self, name: str, description: str = ""):
        self.name = name
        self.description = description
        self.func = None

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            return func(*args, **kwargs)
        wrapper._cmd_name = self.name
        wrapper._cmd_description = self.description
        wrapper._is_command = True
        return wrapper


# =============================================================
# 插件基类
# =============================================================

class Plugin:
    """
    插件基类，所有 Python 插件应继承此类

    属性:
        name (str): 插件名称（必须）
        version (str): 插件版本
        description (str): 插件描述
        author (str): 作者
        type (str): 插件类型 (skill/command/hook/data_source/ui)

    方法:
        on_load(): 插件加载时调用
        on_start(): 插件启动时调用
        on_stop(): 插件停止时调用
        on_unload(): 插件卸载时调用
        get_skills(): 返回技能字典
        get_commands(): 返回命令字典
    """
    def __init__(self):
        self.name = ""
        self.version = "1.0.0"
        self.description = ""
        self.author = "Unknown"
        self.type = "skill"  # skill, command, hook, data_source, ui
        self._loaded = False
        self._started = False

    def on_load(self) -> bool:
        """插件加载时调用"""
        logger.info(f"Plugin {self.name} loaded")
        return True

    def on_start(self) -> bool:
        """插件启动时调用"""
        logger.info(f"Plugin {self.name} started")
        return True

    def on_stop(self) -> bool:
        """插件停止时调用"""
        logger.info(f"Plugin {self.name} stopped")
        return True

    def on_unload(self) -> bool:
        """插件卸载时调用"""
        logger.info(f"Plugin {self.name} unloaded")
        return True

    def get_skills(self) -> Dict[str, Dict]:
        """
        获取插件提供的技能列表

        返回格式:
            {
                "skill_name": {
                    "func": callable,
                    "risk": "low",
                    "description": "...",
                    "need_confirm": False
                }
            }
        """
        skills = {}
        for attr_name in dir(self):
            attr = getattr(self, attr_name)
            if callable(attr) and getattr(attr, "_is_skill", False):
                skill_name = getattr(attr, "_skill_name", attr_name)
                skills[skill_name] = {
                    "func": attr,
                    "risk": getattr(attr, "_skill_risk", "low"),
                    "description": getattr(attr, "_skill_description", ""),
                    "need_confirm": getattr(attr, "_skill_need_confirm", False)
                }
        return skills

    def get_commands(self) -> Dict[str, Dict]:
        """
        获取插件提供的命令列表

        返回格式:
            {
                "cmd_name": {
                    "func": callable,
                    "description": "..."
                }
            }
        """
        commands = {}
        for attr_name in dir(self):
            attr = getattr(self, attr_name)
            if callable(attr) and getattr(attr, "_is_command", False):
                cmd_name = getattr(attr, "_cmd_name", attr_name)
                commands[cmd_name] = {
                    "func": attr,
                    "description": getattr(attr, "_cmd_description", "")
                }
        return commands

    def get_info(self) -> Dict:
        """获取插件信息"""
        return {
            "name": self.name,
            "version": self.version,
            "description": self.description,
            "author": self.author,
            "type": self.type,
            "loaded": self._loaded,
            "started": self._started
        }

    def __str__(self) -> str:
        return f"Plugin(name={self.name}, version={self.version}, type={self.type})"


# =============================================================
# 插件管理辅助函数
# =============================================================

def is_plugin_class(cls) -> bool:
    """检查是否为有效的插件类"""
    if not inspect.isclass(cls):
        return False
    if not issubclass(cls, Plugin):
        return False
    if cls is Plugin:
        return False
    return True


def get_plugin_skills(plugin: Plugin) -> Dict[str, Dict]:
    """获取插件技能（已注册）"""
    return plugin.get_skills()


def get_plugin_commands(plugin: Plugin) -> Dict[str, Dict]:
    """获取插件命令（已注册）"""
    return plugin.get_commands()


# =============================================================
# 示例插件模板（供开发者参考）
# =============================================================

"""
# 示例插件: example_plugin.py

from lingos_plugin import Plugin, Skill, Command

class ExamplePlugin(Plugin):
    def __init__(self):
        super().__init__()
        self.name = "example_plugin"
        self.version = "1.0.0"
        self.description = "Example plugin for LING OS"
        self.author = "LING OS Team"
        self.type = "skill"

    def on_load(self):
        print("Example plugin loaded!")
        return True

    @Skill(name="example_skill", risk="low", description="Example skill")
    def example_skill(self, args_json):
        args = json.loads(args_json)
        name = args.get("name", "World")
        return {"result": f"Hello, {name}!"}

    @Command(name="example_cmd", description="Example command")
    def example_cmd(self, args):
        return "Hello from example command!"
"""