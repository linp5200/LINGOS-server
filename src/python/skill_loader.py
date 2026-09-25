#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Skill Loader (外置技能加载器)
版本: LN-B-5.0.0.0
功能：从注册表加载外置技能定义，注册到 skill_handlers 的 SKILL_REGISTRY
      支持热加载和降级（注册表不可用时 fallback 到硬编码）
"""

import os
import sys
import json
import logging
import importlib
import importlib.util
from typing import Dict, Any, Optional, Callable, Tuple

# ========== 导入依赖 ==========
from registry_client import get_registry_client, registry_list, registry_get
from syscall_client import call_syscall

logger = logging.getLogger("SkillLoader")

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def load_skill_language():
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

load_skill_language()

# =============================================================
# 技能执行器工厂
# =============================================================

class SkillExecutor:
    """技能执行器：根据定义类型调用对应的执行逻辑"""

    @staticmethod
    def create(definition: Dict) -> Callable:
        """
        根据技能定义创建执行函数
        :param definition: 技能定义（来自注册表 JSON）
        :return: 可调用函数 (args_json) -> Tuple[bool, str]
        """
        handler_type = definition.get("handler", "python")

        if handler_type == "python":
            # Python 函数：动态导入
            handler_path = definition.get("handler_path", "")
            return SkillExecutor._create_python_executor(handler_path)

        elif handler_type == "syscall":
            # 系统调用：调用 syscall_client
            operation = definition.get("operation", "")
            return SkillExecutor._create_syscall_executor(operation)

        elif handler_type == "shell":
            # Shell 命令：通过 exec_command 执行
            command = definition.get("command", "")
            return SkillExecutor._create_shell_executor(command)

        else:
            # 未知类型：返回空实现
            logger.warning(f"Unknown handler type: {handler_type}")
            return lambda args: (False, t(f"Unknown handler type: {handler_type}", f"未知的处理器类型：{handler_type}"))

    @staticmethod
    def _create_python_executor(handler_path: str) -> Callable:
        """创建 Python 函数执行器（【0.6.0】多策略解析：模块导入 → 技能目录文件加载）

        背景：外置技能（skill_store / 技能包）的 handler_path 通常是技能名，
        其实现文件位于 /LINGOS/skills/enabled/<name>/ 等目录——直接
        import_module 必失败（不在 sys.path）。本实现按顺序尝试：
          ① 模块导入（内置 handler：如 skill_handlers.xxx 或 module.func）
          ② 技能目录文件加载（enabled / skills / registry/skills/* 各目录，
             实现文件候选：<name>.py / handler.py / main.py）
        入口函数按序探测：指定 func → run → handle → execute → main
        """
        if not handler_path:
            return lambda args: (False, t("Missing handler_path", "缺少 handler_path"))

        try:
            module_name, func_name = handler_path.rsplit(".", 1) if "." in handler_path else (handler_path, None)
            wanted = ([func_name] if func_name else []) + ["run", "handle", "execute", "main"]

            # 策略①：模块导入（内置 handler）
            try:
                module = importlib.import_module(module_name)
                for fn in wanted:
                    func = getattr(module, fn, None)
                    if func and callable(func):
                        logger.debug(f"Loaded Python skill (module): {handler_path} -> {fn}")
                        return func
            except ImportError:
                pass
            except Exception as e:
                logger.debug(f"Module load error for {module_name}: {e}")

            # 策略②：技能目录文件加载
            base_dirs = [
                "/LINGOS/skills/enabled",           # skill_store 启用区
                "/LINGOS/skills",                    # 技能包区
                "/LINGOS/registry/skills/custom",    # 注册表自定义区
                "/LINGOS/registry/skills/store",     # 注册表商店区
                "/LINGOS/registry/skills/builtin",   # 注册表内置区
            ]
            file_candidates = []
            for bd in base_dirs:
                skill_dir = os.path.join(bd, handler_path)
                for fname in ("%s.py" % handler_path, "handler.py", "main.py"):
                    p = os.path.join(skill_dir, fname)
                    if os.path.isfile(p):
                        file_candidates.append(p)
                p2 = os.path.join(bd, handler_path + ".py")   # 直接平铺文件
                if os.path.isfile(p2):
                    file_candidates.append(p2)

            for fp in file_candidates:
                try:
                    spec = importlib.util.spec_from_file_location("_lingos_skill_%s" % module_name, fp)
                    if not spec or not spec.loader:
                        continue
                    mod = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(mod)
                    for fn in wanted:
                        func = getattr(mod, fn, None)
                        if func and callable(func):
                            logger.info(f"Loaded Python skill (file): {handler_path} -> {fp}")
                            return func
                except Exception as e:
                    logger.debug(f"file load failed {fp}: {e}")
                    continue

            logger.warning(f"Cannot resolve python skill handler: {handler_path}")
            return lambda args: (False, t(
                f"Skill handler not found: {handler_path}",
                f"技能实现未找到：{handler_path}（请检查技能包完整性）"))

        except Exception as e:
            # 【0.7.0-hf2】except 块结束后 e 被删除——lambda 延迟调用会 NameError
            _err_text = str(e)
            logger.error(f"Failed to create Python executor: {e}")
            return lambda args: (False, _err_text)

    @staticmethod
    def _eval_python_code(code: str, args_json: str) -> Tuple[bool, str]:
        """执行 Python 代码字符串（降级方案）"""
        try:
            # 提供安全的执行环境
            safe_globals = {
                "__builtins__": {
                    "print": print,
                    "len": len,
                    "str": str,
                    "int": int,
                    "float": float,
                    "list": list,
                    "dict": dict,
                    "json": json,
                },
                "args_json": args_json,
            }
            try:
                args = json.loads(args_json)
                safe_globals["args"] = args
            except:
                safe_globals["args"] = {}

            result = eval(code, safe_globals, {})
            if isinstance(result, dict):
                return True, json.dumps(result, ensure_ascii=False)
            return True, str(result)

        except Exception as e:
            logger.error(f"Eval error: {e}")
            return False, str(e)

    @staticmethod
    def _create_syscall_executor(operation: str) -> Callable:
        """创建系统调用执行器"""
        def executor(args_json: str) -> Tuple[bool, str]:
            try:
                args = json.loads(args_json) if args_json else {}
                success, result = call_syscall(operation, args)
                return success, result
            except Exception as e:
                return False, str(e)
        return executor

    @staticmethod
    def _create_shell_executor(command: str) -> Callable:
        """创建 Shell 命令执行器"""
        def executor(args_json: str) -> Tuple[bool, str]:
            try:
                args = json.loads(args_json) if args_json else {}
                # 替换命令中的占位符 {key}
                cmd = command
                for key, val in args.items():
                    cmd = cmd.replace(f"{{{key}}}", str(val))
                success, result = call_syscall("exec_command", {"command": cmd})
                return success, result
            except Exception as e:
                return False, str(e)
        return executor


# =============================================================
# 技能加载器
# =============================================================

class SkillLoader:
    """外置技能加载器（从注册表加载）"""

    _instance = None
    _loaded = False
    _skill_count = 0

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(SkillLoader, cls).__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._registry_client = get_registry_client()
        self._loaded_skills = {}
        logger.info("SkillLoader initialized")

    def load_all(self, target_registry: Dict = None) -> int:
        """
        从注册表加载所有技能
        :param target_registry: 目标注册表（默认使用 skill_handlers.SKILL_REGISTRY）
        :return: 加载的技能数量
        """
        logger.info("Loading skills from registry...")

        # 获取目标注册表
        if target_registry is None:
            try:
                from skill_handlers import SKILL_REGISTRY
                target_registry = SKILL_REGISTRY
            except ImportError:
                logger.error("Cannot import skill_handlers.SKILL_REGISTRY")
                return 0

        # 从注册表获取技能列表
        skill_entries = registry_list("skill")
        if not skill_entries:
            logger.warning("No skills found in registry, using fallback")
            return 0

        count = 0
        for entry in skill_entries:
            # 检查元数据
            metadata = entry.get("metadata", {})
            if isinstance(metadata, str):
                try:
                    metadata = json.loads(metadata)
                except:
                    metadata = {}

            skill_id = entry.get("id", "")
            skill_name = entry.get("name", skill_id)

            # 解析定义
            definition = metadata.get("definition", {})
            if not definition:
                # 尝试从 entry.path 读取 JSON 文件
                definition = self._load_skill_from_file(entry.get("path", ""))
                if not definition:
                    logger.warning(f"Skill {skill_id} has no valid definition")
                    continue

            # 构建技能信息
            executor = SkillExecutor.create(definition)
            risk = definition.get("risk", "low")
            description = definition.get("description", "")
            need_confirm = definition.get("need_confirm", False)

            # 注册到目标注册表
            target_registry[skill_name] = {
                "func": executor,
                "risk": risk,
                "need_confirm": need_confirm,
                "description": description,
                "source": "registry"
            }

            self._loaded_skills[skill_id] = {
                "name": skill_name,
                "definition": definition
            }
            count += 1
            logger.debug(f"Loaded skill: {skill_name} (risk={risk})")

        self._loaded = True
        self._skill_count = count
        logger.info(f"Loaded {count} skills from registry")
        return count

    def _load_skill_from_file(self, path: str) -> Optional[Dict]:
        """从 JSON 文件加载技能定义"""
        if not path or not os.path.exists(path):
            return None

        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            # 提取定义
            if "definition" in data:
                return data["definition"]
            return data
        except Exception as e:
            logger.warning(f"Failed to load skill from {path}: {e}")
            return None

    def reload(self, target_registry: Dict = None) -> int:
        """热重载技能"""
        self._loaded_skills.clear()
        self._loaded = False
        self._skill_count = 0
        return self.load_all(target_registry)

    def get_loaded_count(self) -> int:
        return self._skill_count

    def is_loaded(self) -> bool:
        return self._loaded

    def get_skill(self, skill_id: str) -> Optional[Dict]:
        """获取已加载的技能信息"""
        return self._loaded_skills.get(skill_id)


# =============================================================
# 便捷函数
# =============================================================

_loader = None

def get_loader() -> SkillLoader:
    global _loader
    if _loader is None:
        _loader = SkillLoader()
    return _loader

def load_skills(target_registry: Dict = None) -> int:
    """加载所有外置技能"""
    return get_loader().load_all(target_registry)

def reload_skills(target_registry: Dict = None) -> int:
    """热重载外置技能"""
    return get_loader().reload(target_registry)

def get_skill_count() -> int:
    """获取已加载技能数量"""
    return get_loader().get_loaded_count()