#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Plugin Discovery
版本: LN-B-4.2.0.0
功能：扫描和发现 Python 插件
"""

import os
import sys
import importlib.util
import inspect
import logging
from typing import List, Dict, Optional, Tuple, Any
from pathlib import Path

# ========== 日志配置 ==========
logger = logging.getLogger("PluginDiscovery")

# ========== 常量 ==========
PLUGIN_DIRS = [
    "/LINGOS/plugins/python",
    "/LINGOS/plugins/custom/python",
]

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en


# =============================================================
# 插件发现核心
# =============================================================

def discover_plugins(plugin_dirs: Optional[List[str]] = None) -> List[Dict]:
    """
    发现所有可用 Python 插件

    Args:
        plugin_dirs: 插件目录列表（默认使用 PLUGIN_DIRS）

    Returns:
        插件信息列表，每个元素包含:
            {
                "path": str,        # 文件路径
                "name": str,        # 模块名称
                "class_name": str,  # 插件类名称
                "module": module,   # 已加载模块（或 None）
                "error": str        # 错误信息（如果有）
            }
    """
    if plugin_dirs is None:
        plugin_dirs = PLUGIN_DIRS

    logger.info(f"discover_plugins: scanning {len(plugin_dirs)} directories")
    plugins = []

    for plugin_dir in plugin_dirs:
        # 确保目录存在
        if not os.path.exists(plugin_dir):
            logger.debug(f"discover_plugins: {plugin_dir} does not exist, creating")
            try:
                os.makedirs(plugin_dir, exist_ok=True)
                logger.debug(f"discover_plugins: created {plugin_dir}")
            except Exception as e:
                logger.warning(f"discover_plugins: cannot create {plugin_dir}: {e}")
                continue

        # 遍历目录中的 Python 文件
        try:
            for file in os.listdir(plugin_dir):
                if not file.endswith('.py') or file.startswith('_') or file == '__init__.py':
                    continue

                file_path = os.path.join(plugin_dir, file)
                module_name = file[:-3]  # 去掉 .py

                logger.debug(f"discover_plugins: found {file_path}")

                # 尝试加载模块以获取插件类信息
                plugin_info = load_plugin_module(file_path, module_name)
                if plugin_info:
                    plugins.append(plugin_info)

        except Exception as e:
            logger.error(f"discover_plugins: error scanning {plugin_dir}: {e}")

    logger.info(f"discover_plugins: found {len(plugins)} plugins")
    return plugins


def load_plugin_module(file_path: str, module_name: str) -> Optional[Dict]:
    """
    加载单个插件模块并提取插件类信息

    Returns:
        插件信息字典，或 None（如果加载失败）
    """
    try:
        # 加载模块
        spec = importlib.util.spec_from_file_location(module_name, file_path)
        if spec is None or spec.loader is None:
            return None

        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        # 查找 Plugin 子类
        plugin_class = None
        for attr_name in dir(module):
            attr = getattr(module, attr_name)
            if (inspect.isclass(attr) and
                issubclass(attr, object) and
                hasattr(attr, '__module__') and
                attr.__module__ == module_name):
                # 检查是否是 Plugin 的子类（避免导入 lingos_plugin 本身）
                if (hasattr(attr, 'name') or
                    hasattr(attr, 'get_skills') or
                    hasattr(attr, 'get_commands')):
                    # 简单检查：有 name 属性或 get_skills 方法
                    if not hasattr(attr, 'on_load'):  # 简单启发式
                        # 更进一步检查
                        pass
                    # 通过 import 检查是否继承 Plugin
                    try:
                        from lingos_plugin import Plugin
                        if issubclass(attr, Plugin):
                            plugin_class = attr
                            break
                    except ImportError:
                        # 如果无法导入 Plugin，检查是否有 name 和 get_skills
                        if hasattr(attr, 'name') and hasattr(attr, 'get_skills'):
                            plugin_class = attr
                            break

        if plugin_class is None:
            return None

        return {
            "path": file_path,
            "name": module_name,
            "class_name": plugin_class.__name__,
            "module": module,
            "class": plugin_class,
            "error": None
        }

    except Exception as e:
        logger.error(f"load_plugin_module: error loading {file_path}: {e}")
        return {
            "path": file_path,
            "name": module_name,
            "class_name": None,
            "module": None,
            "class": None,
            "error": str(e)
        }


def get_plugin_class_name(file_path: str) -> Optional[str]:
    """
    快速获取插件类名（不加载模块）
    """
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()

        # 简单启发式：查找 class ...(Plugin) 或 class ...(BasePlugin)
        import re
        patterns = [
            r'class\s+(\w+)\s*\(\s*Plugin\s*\)',
            r'class\s+(\w+)\s*\(\s*BasePlugin\s*\)',
            r'class\s+(\w+)\s*:\s*\n\s*#\s*Plugin',
        ]
        for pattern in patterns:
            match = re.search(pattern, content)
            if match:
                return match.group(1)
        return None
    except Exception:
        return None


# =============================================================
# 便捷函数
# =============================================================

def get_plugin_dirs() -> List[str]:
    """获取默认插件目录列表"""
    return PLUGIN_DIRS


def ensure_plugin_dirs() -> None:
    """确保所有插件目录存在"""
    for dir_path in PLUGIN_DIRS:
        try:
            os.makedirs(dir_path, exist_ok=True)
        except Exception as e:
            logger.warning(f"ensure_plugin_dirs: cannot create {dir_path}: {e}")