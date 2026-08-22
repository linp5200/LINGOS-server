#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Skill Install（技能安装管理——2026-08-22 定稿 OpenClaw 式）
版本: LN-B-5.1.2.6-rc
功能：
  - 技能包格式：目录（或 git 仓库）含 SKILL.md + handler.py + requirements.txt
  - SKILL.md: YAML front-matter（name/description/risk）+ 描述正文
  - 内置技能（BUILTIN 标记）不可删除；自定义技能可增删
  - 安装：复制到 SKILLS_ROOT/<name> → 注册到 skill_handlers.SKILL_REGISTRY
  - 调用适配：读取技能自带 schema，自动注册（无需手写适配）
核心协议：C-C 防弹/防御/容错/跛脚 + C1 分级日志
"""

import os
import re
import json
import shutil
import logging
import importlib.util
from typing import Dict, Optional, Tuple, List

logger = logging.getLogger("SkillInstall")

SKILLS_ROOT = "/LINGOS/skills"
BUILTIN_MARK = ".builtin"          # 内置技能标记文件（存在=内置，不可删除）
SKILL_MD = "SKILL.md"
HANDLER_PY = "handler.py"
REQUIREMENTS = "requirements.txt"

# =============================================================
# SKILL.md 解析（YAML front-matter 简易版——零依赖）
# 格式：
#   ---
#   name: my_skill
#   description: ...
#   risk: low
#   handler: python      (python/shell/syscall，默认 python)
#   ---
#   描述正文...
# =============================================================

_FRONT_MATTER_RE = re.compile(r"^---\s*\n(.*?)\n---\s*\n?(.*)$", re.DOTALL)

def parse_skill_md(path: str) -> Optional[Dict]:
    """解析 SKILL.md → {name, description, risk, handler, body}"""
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        m = _FRONT_MATTER_RE.match(text)
        meta: Dict = {}
        body = text
        if m:
            raw = m.group(1)
            body = (m.group(2) or "").strip()
            for line in raw.splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if ":" in line:
                    k, v = line.split(":", 1)
                    meta[k.strip().lower()] = v.strip().strip('"\'')
        name = meta.get("name", os.path.basename(os.path.dirname(path)))
        risk = meta.get("risk", "low")
        if risk not in ("low", "medium", "high"):
            risk = "low"
        handler = meta.get("handler", "python")
        if handler not in ("python", "shell", "syscall"):
            handler = "python"
        return {
            "name": name,
            "description": meta.get("description", body[:200] if body else ""),
            "risk": risk,
            "handler": handler,
            "body": body,
        }
    except Exception as e:
        logger.warning("parse_skill_md failed: %s: %s", path, e)
        return None

# =============================================================
# 扫描/安装/删除
# =============================================================

def scan_skills() -> List[Dict]:
    """扫描 SKILLS_ROOT 下所有技能 → [{name, builtin, path, desc}]"""
    result: List[Dict] = []
    if not os.path.isdir(SKILLS_ROOT):
        return result
    try:
        for entry in sorted(os.listdir(SKILLS_ROOT)):
            sdir = os.path.join(SKILLS_ROOT, entry)
            if not os.path.isdir(sdir):
                continue
            md_path = os.path.join(sdir, SKILL_MD)
            meta = parse_skill_md(md_path) if os.path.exists(md_path) else None
            builtin = os.path.exists(os.path.join(sdir, BUILTIN_MARK))
            result.append({
                "name": entry,
                "builtin": builtin,
                "path": sdir,
                "description": (meta or {}).get("description", ""),
                "risk": (meta or {}).get("risk", "low"),
                "has_handler": os.path.exists(os.path.join(sdir, HANDLER_PY)),
                "has_requirements": os.path.exists(os.path.join(sdir, REQUIREMENTS)),
            })
    except Exception as e:
        logger.warning("scan_skills failed: %s", e)
    return result

def install_skill(src_dir: str) -> Tuple[bool, str]:
    """安装技能：复制 src_dir → SKILLS_ROOT/<name>，返回 (ok, msg)"""
    if not src_dir or not os.path.isdir(src_dir):
        return False, "invalid source directory"
    md_path = os.path.join(src_dir, SKILL_MD)
    if not os.path.exists(md_path):
        return False, "skill package must contain SKILL.md"
    meta = parse_skill_md(md_path)
    if not meta or not meta["name"]:
        return False, "SKILL.md missing valid 'name'"
    name = meta["name"]
    # 安全校验：技能名只允许字母数字下划线（防路径穿越）
    if not re.match(r"^[A-Za-z0-9_]{1,64}$", name):
        return False, "skill name must match [A-Za-z0-9_]{1,64}"
    dst = os.path.join(SKILLS_ROOT, name)
    try:
        os.makedirs(SKILLS_ROOT, exist_ok=True)
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(src_dir, dst)
        # 校验依赖声明
        reqs = os.path.join(dst, REQUIREMENTS)
        if os.path.exists(reqs):
            with open(reqs, "r", encoding="utf-8") as f:
                deps = [l.strip() for l in f if l.strip() and not l.strip().startswith("#")]
            if deps:
                logger.info("skill %s declares deps: %s (install via pip if missing)", name, ", ".join(deps))
        logger.info("skill installed: %s", name)
        return True, "installed: %s" % name
    except Exception as e:
        logger.error("install_skill failed: %s", e)
        return False, "install failed: %s" % str(e)

def remove_skill(name: str) -> Tuple[bool, str]:
    """删除技能（内置不可删）"""
    if not name or not re.match(r"^[A-Za-z0-9_]{1,64}$", name):
        return False, "invalid skill name"
    dst = os.path.join(SKILLS_ROOT, name)
    if not os.path.isdir(dst):
        return False, "skill not found: %s" % name
    if os.path.exists(os.path.join(dst, BUILTIN_MARK)):
        return False, "builtin skill cannot be removed"
    try:
        shutil.rmtree(dst)
        logger.info("skill removed: %s", name)
        return True, "removed: %s" % name
    except Exception as e:
        return False, "remove failed: %s" % str(e)

# =============================================================
# 注册到 skill_handlers（调用适配——自动注册无需手写）
# =============================================================

def load_custom_skills(target_registry: Dict = None) -> int:
    """扫描 SKILLS_ROOT 自定义技能，注册到 SKILL_REGISTRY
    handler.py 提供 run(args_json) -> (bool, str)；或经 SkillExecutor"""
    if target_registry is None:
        try:
            from skill_handlers import SKILL_REGISTRY
            target_registry = SKILL_REGISTRY
        except Exception as e:
            logger.warning("cannot import SKILL_REGISTRY: %s", e)
            return 0
    count = 0
    for skill in scan_skills():
        name = skill["name"]
        if not skill["has_handler"]:
            continue
        if name in target_registry:
            continue  # 同名内置/已注册优先，不覆盖
        sdir = skill["path"]
        handler_path = os.path.join(sdir, HANDLER_PY)

        def make_executor(hp):
            def executor(args_json: str) -> Tuple[bool, str]:
                try:
                    spec = importlib.util.spec_from_file_location("_skill_h", hp)
                    if not spec or not spec.loader:
                        return False, "handler load error"
                    mod = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(mod)
                    if not hasattr(mod, "run"):
                        return False, "handler.py must define run(args_json) -> (bool, str)"
                    return mod.run(args_json)
                except Exception as e:
                    return False, "skill handler error: %s" % str(e)
            return executor

        target_registry[name] = {
            "func": make_executor(handler_path),
            "description": skill["description"],
            "risk": skill["risk"],
            "source": "custom_skill",
        }
        count += 1
        logger.info("registered custom skill: %s (risk=%s)", name, skill["risk"])
    return count


# 便捷：reload 入口（供 ai_server reload_skills 调用）
def reload_custom_skills(target_registry: Dict = None) -> int:
    return load_custom_skills(target_registry)
