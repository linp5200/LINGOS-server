#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Python 插件 SDK
版本: LN-B-4.3.0.0
功能：Python 插件基类和装饰器
"""

import os
import sys
import json
import logging
from typing import Dict, Any, Callable, Optional, List, Tuple
from functools import wraps

logger = logging.getLogger("PluginSDK")


class Plugin:
    """
    Python 插件基类
    所有 Python 插件必须继承此类
    """

    def __init__(self):
        self.name = ""
        self.version = "1.0.0"
        self.description = ""
        self.author = "Unknown"
        self.type = "skill"  # skill, command, data_source, hook, ui_widget
        self._loaded = False

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
        """获取插件提供的技能"""
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
        """获取插件提供的命令"""
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
            "loaded": self._loaded
        }


class Skill:
    """
    技能装饰器
    用法:
        @Skill(name="my_skill", risk="low", description="My skill")
        def my_skill(args):
            return {"result": "success"}
    """

    def __init__(self, name: str, risk: str = "low", description: str = "", need_confirm: bool = False):
        self.name = name
        self.risk = risk
        self.description = description
        self.need_confirm = need_confirm

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            return func(*args, **kwargs)
        wrapper._is_skill = True
        wrapper._skill_name = self.name
        wrapper._skill_risk = self.risk
        wrapper._skill_description = self.description
        wrapper._skill_need_confirm = self.need_confirm
        return wrapper


class Command:
    """
    命令装饰器
    用法:
        @Command(name="mycmd", description="My command")
        def my_cmd(args):
            return "Hello"
    """

    def __init__(self, name: str, description: str = ""):
        self.name = name
        self.description = description

    def __call__(self, func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            return func(*args, **kwargs)
        wrapper._is_command = True
        wrapper._cmd_name = self.name
        wrapper._cmd_description = self.description
        return wrapper