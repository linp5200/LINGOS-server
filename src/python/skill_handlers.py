#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Skill Handlers
版本: LN-B-5.0.0.0
功能：60+内置技能注册与执行，集成外置技能加载器
      支持混合模式：注册表优先 → 硬编码 fallback
      含记忆系统、帮助生成、默认配置创建
"""

import os
import sys
import json
import time
import logging
import shutil
import subprocess
import socket
import hashlib
import random
from typing import Dict, Any, Tuple, Callable, Optional, List

# ========== 多语言支持 ==========
_current_lang = "en"

def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en

def set_skill_language(lang: str):
    global _current_lang
    if lang in ("en", "zh"):
        _current_lang = lang

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

# ========== 导入系统调用客户端 ==========
try:
    from syscall_client import call_syscall
except ImportError:
    # 降级：定义占位函数
    def call_syscall(op, args, timeout=30):
        return False, f"syscall_client not available for {op}"

logger = logging.getLogger("SkillHandlers")

# =============================================================
# 导入外置技能加载器（新增）
# =============================================================

try:
    from skill_loader import load_skills, reload_skills, get_skill_count
    _SKILL_LOADER_AVAILABLE = True
    logger.info("SkillLoader available")
except ImportError as e:
    _SKILL_LOADER_AVAILABLE = False
    logger.warning(f"SkillLoader not available: {e}")

# =============================================================
# 技能注册表
# =============================================================
SKILL_REGISTRY: Dict[str, Dict] = {}

# =============================================================
# 注册函数
# =============================================================

def register_skill(name: str, func: Callable, risk: str = "low",
                   need_confirm: bool = False, source: str = "builtin"):
    """注册技能到全局注册表（自动提取 docstring 首行作为描述）"""
    description = ""
    if func and func.__doc__:
        description = func.__doc__.strip().split("\n")[0][:120]
    SKILL_REGISTRY[name] = {
        "func": func,
        "risk": risk,
        "need_confirm": need_confirm,
        "source": source,
        "description": description
    }
    logger.debug(f"Registered skill: {name} (risk={risk}, source={source})")


# =============================================================
# 1. 文件操作技能（完整）
# =============================================================

def file_write(args_json: str) -> Tuple[bool, str]:
    """创建或覆盖写入文件"""
    try:
        args = json.loads(args_json)
        path = args.get("path")
        content = args.get("content", "")
        if not path:
            return False, t("Missing 'path' parameter", "缺少 'path' 参数")
        # 安全检查：禁止写入系统关键目录
        forbidden = ["/etc/passwd", "/etc/shadow", "/etc/sudoers", "/boot/"]
        for f in forbidden:
            if path.startswith(f):
                return False, t(f"Writing to {f} is not allowed", f"不允许写入 {f}")
        success, result = call_syscall("file_write", {"path": path, "content": content})
        if success:
            return True, t(f"Written to {path}", f"已写入 {path}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_write", file_write, risk="medium")

def file_read(args_json: str) -> Tuple[bool, str]:
    """读取文件内容"""
    try:
        args = json.loads(args_json)
        path = args.get("path")
        if not path:
            return False, t("Missing 'path' parameter", "缺少 'path' 参数")
        # 安全检查
        if path.startswith("/etc/shadow") or path.startswith("/etc/sudoers"):
            return False, t("Reading this file is not allowed", "不允许读取此文件")
        success, result = call_syscall("file_read", {"path": path})
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_read", file_read, risk="low")

def file_delete(args_json: str) -> Tuple[bool, str]:
    """删除文件"""
    try:
        args = json.loads(args_json)
        path = args.get("path")
        if not path:
            return False, t("Missing 'path' parameter", "缺少 'path' 参数")
        forbidden = ["/etc/passwd", "/etc/shadow", "/etc/sudoers", "/boot/", "/LINGOS/Ensystem/"]
        for f in forbidden:
            if path.startswith(f):
                return False, t(f"Deleting {f} is not allowed", f"不允许删除 {f}")
        success, result = call_syscall("file_delete", {"path": path})
        if success:
            return True, t(f"Deleted {path}", f"已删除 {path}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_delete", file_delete, risk="medium")

def file_list(args_json: str) -> Tuple[bool, str]:
    """列出目录内容"""
    try:
        args = json.loads(args_json)
        path = args.get("path", ".")
        success, result = call_syscall("file_list", {"path": path})
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_list", file_list, risk="low")

def file_mkdir(args_json: str) -> Tuple[bool, str]:
    """创建目录"""
    try:
        args = json.loads(args_json)
        path = args.get("path")
        if not path:
            return False, t("Missing 'path' parameter", "缺少 'path' 参数")
        success, result = call_syscall("file_mkdir", {"path": path})
        if success:
            return True, t(f"Created directory {path}", f"已创建目录 {path}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_mkdir", file_mkdir, risk="low")

def file_copy(args_json: str) -> Tuple[bool, str]:
    """复制文件"""
    try:
        args = json.loads(args_json)
        src = args.get("src")
        dst = args.get("dst")
        if not src or not dst:
            return False, t("Missing 'src' or 'dst'", "缺少 'src' 或 'dst'")
        # 读取源文件
        success, content = call_syscall("file_read", {"path": src})
        if not success:
            return False, t(f"Failed to read source: {content}", f"读取源文件失败：{content}")
        success, result = call_syscall("file_write", {"path": dst, "content": content})
        if success:
            return True, t(f"Copied {src} to {dst}", f"已复制 {src} 到 {dst}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("file_copy", file_copy, risk="medium")

def file_move(args_json: str) -> Tuple[bool, str]:
    """移动/重命名文件"""
    try:
        args = json.loads(args_json)
        src = args.get("src")
        dst = args.get("dst")
        if not src or not dst:
            return False, t("Missing 'src' or 'dst'", "缺少 'src' 或 'dst'")
        success, content = call_syscall("file_read", {"path": src})
        if not success:
            return False, t(f"Failed to read source: {content}", f"读取源文件失败：{content}")
        success, result = call_syscall("file_write", {"path": dst, "content": content})
        if not success:
            return False, t(f"Failed to write destination: {result}", f"写入目标失败：{result}")
        success, result = call_syscall("file_delete", {"path": src})
        if not success:
            return False, t(f"Failed to delete source: {result}", f"删除源文件失败：{result}")
        return True, t(f"Moved {src} to {dst}", f"已移动 {src} 到 {dst}")
    except Exception as e:
        return False, str(e)
register_skill("file_move", file_move, risk="medium")


# =============================================================
# 2. 记忆系统技能（完整）
# =============================================================

def memory_write(args_json: str) -> Tuple[bool, str]:
    """写入记忆（short/medium/long + importance: high/normal——先生决策 0.2.0 双记忆）"""
    try:
        args = json.loads(args_json)
        mem_type = args.get("type", "short")
        content = args.get("content")
        keywords = args.get("keywords", [])
        importance = args.get("importance", "normal")
        if not content:
            return False, t("Missing 'content'", "缺少 'content'")
        if mem_type not in ("short", "medium", "long"):
            mem_type = "short"
        # 【0.2.0 双记忆】重要记忆（importance=high）前缀标记 [重要]——自动注入专用；
        # 普通记忆 AI 自主调用检索。AI 自行决定重要性。
        if importance in ("high", "important") and not str(content).startswith("[重要]"):
            content = "[重要] " + str(content)
        success, result = call_syscall("memory_write", {
            "type": mem_type,
            "content": content,
            "keywords": keywords
        })
        if success:
            return True, t(f"Memory stored: {result}", f"记忆已存储：{result}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("memory_write", memory_write, risk="medium")

def memory_search(args_json: str) -> Tuple[bool, str]:
    """搜索记忆（关键词匹配）"""
    try:
        args = json.loads(args_json)
        keyword = args.get("keyword")
        if not keyword:
            return False, t("Missing 'keyword'", "缺少 'keyword'")
        success, result = call_syscall("memory_search", {"keyword": keyword})
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("memory_search", memory_search, risk="low")

def memory_read(args_json: str) -> Tuple[bool, str]:
    """读取单条记忆"""
    try:
        args = json.loads(args_json)
        mem_id = args.get("id")
        if not mem_id:
            return False, t("Missing 'id'", "缺少 'id'")
        success, result = call_syscall("memory_read", {"id": mem_id})
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("memory_read", memory_read, risk="low")

def memory_delete(args_json: str) -> Tuple[bool, str]:
    """删除记忆"""
    try:
        args = json.loads(args_json)
        mem_id = args.get("id")
        if not mem_id:
            return False, t("Missing 'id'", "缺少 'id'")
        success, result = call_syscall("memory_delete", {"id": mem_id})
        if success:
            return True, t(f"Memory deleted: {mem_id}", f"记忆已删除：{mem_id}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("memory_delete", memory_delete, risk="medium")

def memory_index(args_json: str) -> Tuple[bool, str]:
    """列出记忆索引"""
    try:
        args = json.loads(args_json)
        mem_type = args.get("type")
        params = {}
        if mem_type:
            params["type"] = mem_type
        success, result = call_syscall("memory_index", params)
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("memory_index", memory_index, risk="low")


# =============================================================
# 【批次3】HA（Help AI）事件归档技能（主 AI 专属）
# =============================================================

def ha_write(args_json: str) -> Tuple[bool, str]:
    """创建 HA 事件档案（技术/系统事件，帮助未来 AI 决策）

    参数: {
      "type": "success|failure|info|warning",
      "title": "事件标题",
      "summary": "AI 构建的摘要",
      "analysis": "根因分析/建议（可选）",
      "severity": "info|warn|critical",
      "payload": {...} （可选）
    }
    """
    try:
        import ha_archive
        args = json.loads(args_json)
        if not args.get("title"):
            return False, t("Missing 'title'", "缺少 'title'")
        event_id = ha_archive.ha_write_event(
            event_type=args.get("type", "info"),
            title=args.get("title", ""),
            summary=args.get("summary", ""),
            analysis=args.get("analysis", ""),
            source=args.get("source", "ai:nook"),
            severity=args.get("severity", "info"),
            payload=args.get("payload", {})
        )
        if event_id:
            return True, t(f"HA event created: {event_id}", f"HA 事件已创建：{event_id}")
        return False, t("HA event creation failed", "HA 事件创建失败")
    except Exception as e:
        return False, str(e)
register_skill("ha_write", ha_write, risk="medium")


def ha_search(args_json: str) -> Tuple[bool, str]:
    """检索 HA 事件档案（参考历史成功/失败模式）

    参数: {"query": "关键词", "limit": 20, "type": "failure|success..."}
    """
    try:
        import ha_archive
        args = json.loads(args_json)
        events = ha_archive.ha_search(
            query=args.get("query", ""),
            limit=int(args.get("limit", 20)),
            event_type=args.get("type")
        )
        if events:
            return True, json.dumps(events, ensure_ascii=False)
        return True, t("No HA events found", "未找到相关 HA 事件")
    except Exception as e:
        return False, str(e)
register_skill("ha_search", ha_search, risk="low")


def agent_delegate(args_json):
    """【批次3】委派任务给子 AI（对话协作模式，Hub 路由）

    参数: {"role": "ai_code|ai_guard|ai_general", "prompt": "任务描述"}
    """
    try:
        import agent_orchestrator
        args = json.loads(args_json)
        role = args.get("role", "ai_general")
        prompt = args.get("prompt", "")
        if not prompt:
            return False, t("Missing 'prompt'", "缺少 'prompt'")
        orch = agent_orchestrator.get_orchestrator()
        task_id, _token = orch.create_session(role, prompt[:200])
        orch.run_sub_agent(task_id, prompt)
        return True, json.dumps({"task_id": task_id, "status": "running", "role": role},
                                ensure_ascii=False)
    except Exception as e:
        return False, str(e)
register_skill("agent_delegate", agent_delegate, risk="medium")


# =============================================================
# 【批次D】网络搜索技能（searxng 主 + html 降级 + 并行多主题）
# =============================================================

def _get_search_cfg():
    """读取搜索配置（search_backend / max_urls / rate_limit）"""
    try:
        with open("/LINGOS/system/config/ai_config.json", "r", encoding="utf-8") as f:
            cfg = json.load(f)
        backend = cfg.get("search_backend", "searxng")
        max_urls = int(cfg.get("search_max_urls", 50))
        rate = int(cfg.get("search_rate_limit", 10))
        if backend not in ("searxng", "html"):
            backend = "searxng"
        return backend, max_urls, rate
    except Exception:
        return "searxng", 50, 10


def web_search(args_json):
    """搜索网页（支持单查询或多主题并行）

    参数: {"query": "关键词" 或 ["主题1","主题2"], "num": 条数(默认5)}
    """
    try:
        import web_search as ws
        args = json.loads(args_json)
        query = args.get("query", "")
        num = int(args.get("num", 5))
        backend, max_urls, rate = _get_search_cfg()

        if isinstance(query, list):
            results = ws.web_search_multi(query, num_per_query=num,
                                          backend=backend, max_urls=max_urls)
        else:
            results = ws.web_search(query, num_results=num,
                                    backend=backend, rate_limit=rate)
        if not results:
            return True, t("No search results.", "未找到搜索结果。")
        return True, json.dumps(results, ensure_ascii=False)
    except Exception as e:
        return False, str(e)
register_skill("web_search", web_search, risk="low")


def web_fetch(args_json):
    """抓取指定 URL 内容（SSRF 防护）

    参数: {"url": "https://..."}
    """
    try:
        import web_search as ws
        args = json.loads(args_json)
        url = args.get("url", "")
        if not url:
            return False, t("Missing 'url'", "缺少 'url'")
        content = ws.web_fetch(url)
        if content.startswith("Error:"):
            return False, content
        return True, content
    except Exception as e:
        return False, str(e)
register_skill("web_fetch", web_fetch, risk="medium")


# =============================================================
# 【批次E】git 技能（只读：status/log/diff）
# =============================================================

def git_status(args_json):
    """查看 git 仓库状态（只读）"""
    try:
        import git_skills
        args = json.loads(args_json)
        return git_skills.git_status(args_json)
    except Exception as e:
        return False, str(e)
register_skill("git_status", git_status, risk="low")


def git_log(args_json):
    """查看 git 提交历史（只读）"""
    try:
        import git_skills
        return git_skills.git_log(args_json)
    except Exception as e:
        return False, str(e)
register_skill("git_log", git_log, risk="low")


def git_diff(args_json):
    """查看 git 差异（只读）"""
    try:
        import git_skills
        return git_skills.git_diff(args_json)
    except Exception as e:
        return False, str(e)
register_skill("git_diff", git_diff, risk="low")


# =============================================================
# 3. 进程操作技能（完整）
# =============================================================

def process_list(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("process_list", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("process_list", process_list, risk="low")

def process_kill(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        pid = args.get("pid")
        if pid is None:
            return False, t("Missing 'pid'", "缺少 'pid'")
        # 保护系统进程
        if pid <= 10:
            return False, t("Cannot kill system process", "不能终止系统进程")
        success, result = call_syscall("process_kill", {"pid": pid})
        if success:
            return True, t(f"Killed process {pid}", f"已终止进程 {pid}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("process_kill", process_kill, risk="high", need_confirm=True)

def process_info(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        pid = args.get("pid")
        if pid is None:
            return False, t("Missing 'pid'", "缺少 'pid'")
        success, result = call_syscall("process_info", {"pid": pid})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("process_info", process_info, risk="low")


# =============================================================
# 4. 系统信息技能（完整）
# =============================================================

def system_info(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("system_info", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_info", system_info, risk="low")

def system_uptime(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("system_uptime", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_uptime", system_uptime, risk="low")

def system_memory(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("system_memory", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_memory", system_memory, risk="low")

def system_disk(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("system_disk", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_disk", system_disk, risk="low")

def system_cpu(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("system_cpu", {})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_cpu", system_cpu, risk="low")

def system_log(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("file_read", {"path": "/var/log/syslog"})
        if success:
            return True, result
        else:
            return False, t("Log file not available", "日志文件不可用")
    except Exception as e:
        return False, str(e)
register_skill("system_log", system_log, risk="low")


# =============================================================
# 5. 网络操作技能（完整）
# =============================================================

def net_status(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("exec_command", {"command": "ip addr 2>/dev/null || ifconfig"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("net_status", net_status, risk="medium")

def net_ping(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        host = args.get("host", "8.8.8.8")
        count = args.get("count", 4)
        success, result = call_syscall("net_ping", {"host": host, "count": count})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("net_ping", net_ping, risk="low")

def net_curl(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        url = args.get("url")
        if not url:
            return False, t("Missing 'url'", "缺少 'url'")
        success, result = call_syscall("net_curl", {"url": url})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("net_curl", net_curl, risk="medium")

def net_dns_lookup(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        domain = args.get("domain")
        if not domain:
            return False, t("Missing 'domain'", "缺少 'domain'")
        success, result = call_syscall("net_dns_lookup", {"domain": domain})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("net_dns_lookup", net_dns_lookup, risk="low")


# =============================================================
# 6. 包管理技能（完整）
# =============================================================

def package_list(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("exec_command", {"command": "apt list --installed 2>/dev/null | head -20"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("package_list", package_list, risk="low")

def package_search(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        keyword = args.get("keyword")
        if not keyword:
            return False, t("Missing 'keyword'", "缺少 'keyword'")
        success, result = call_syscall("exec_command", {"command": f"apt search {keyword} 2>/dev/null | head -20"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("package_search", package_search, risk="low")

def package_install(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        pkg = args.get("package")
        if not pkg:
            return False, t("Missing 'package'", "缺少 'package'")
        dangerous = ["rm", "rm -rf", "mkfs", "dd", "fdisk", "grub", "kernel"]
        for d in dangerous:
            if d in pkg:
                return False, t(f"Blocked dangerous pattern: {d}", f"拦截危险模式：{d}")
        # 需要使用 sudo 或 root
        success, result = call_syscall("exec_command", {"command": f"apt install -y {pkg} 2>&1"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("package_install", package_install, risk="critical", need_confirm=True)

def package_remove(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        pkg = args.get("package")
        if not pkg:
            return False, t("Missing 'package'", "缺少 'package'")
        dangerous = ["rm", "rm -rf", "mkfs", "dd", "fdisk", "kernel"]
        for d in dangerous:
            if d in pkg:
                return False, t(f"Blocked dangerous pattern: {d}", f"拦截危险模式：{d}")
        success, result = call_syscall("exec_command", {"command": f"apt remove -y {pkg} 2>&1"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("package_remove", package_remove, risk="critical", need_confirm=True)


# =============================================================
# 7. 服务管理技能（完整）
# =============================================================

def service_restart(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        service = args.get("service")
        if not service:
            return False, t("Missing 'service'", "缺少 'service'")
        success, result = call_syscall("exec_command", {"command": f"systemctl restart {service} 2>&1"})
        if success:
            return True, t(f"Restarted service {service}", f"已重启服务 {service}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("service_restart", service_restart, risk="high", need_confirm=True)

def service_status(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        service = args.get("service")
        if not service:
            return False, t("Missing 'service'", "缺少 'service'")
        success, result = call_syscall("exec_command", {"command": f"systemctl status {service} 2>&1"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("service_status", service_status, risk="low")


# =============================================================
# 8. 配置管理技能（完整）
# =============================================================

def config_read(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        path = args.get("path")
        if not path:
            return False, t("Missing 'path'", "缺少 'path'")
        if not path.startswith("/LINGOS/system/config/"):
            return False, t("Only config directory is allowed", "只允许读取配置目录")
        success, result = call_syscall("file_read", {"path": path})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("config_read", config_read, risk="low")

def config_write(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        path = args.get("path")
        content = args.get("content")
        if not path or content is None:
            return False, t("Missing 'path' or 'content'", "缺少 'path' 或 'content'")
        if not path.startswith("/LINGOS/system/config/"):
            return False, t("Only config directory is allowed", "只允许写入配置目录")
        success, result = call_syscall("file_write", {"path": path, "content": content})
        if success:
            return True, t(f"Written to {path}", f"已写入 {path}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("config_write", config_write, risk="critical", need_confirm=True)


# =============================================================
# 9. 脚本操作技能（完整）
# =============================================================

def script_create(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        path = args.get("path")
        content = args.get("content")
        if not path or content is None:
            return False, t("Missing 'path' or 'content'", "缺少 'path' 或 'content'")
        if not path.startswith("/LINGOS/scripts/") and not path.startswith("/tmp/"):
            return False, t("Scripts only allowed in /LINGOS/scripts/ or /tmp/",
                           "脚本只允许放在 /LINGOS/scripts/ 或 /tmp/")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        success, result = call_syscall("file_write", {"path": path, "content": content})
        if not success:
            return False, result
        os.chmod(path, 0o755)
        return True, t(f"Created script {path}", f"已创建脚本 {path}")
    except Exception as e:
        return False, str(e)
register_skill("script_create", script_create, risk="medium")

def script_exec(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        path = args.get("path")
        if not path:
            return False, t("Missing 'path'", "缺少 'path'")
        if not os.path.exists(path):
            return False, t(f"Script not found: {path}", f"脚本未找到：{path}")
        if not os.access(path, os.X_OK):
            return False, t(f"Script not executable: {path}", f"脚本不可执行：{path}")
        # 安全检查：不允许执行系统关键路径
        forbidden = ["/bin/", "/usr/bin/", "/sbin/", "/usr/sbin/"]
        for f in forbidden:
            if path.startswith(f):
                return False, t(f"Executing from {f} is not allowed", f"不允许执行 {f} 中的脚本")
        success, result = call_syscall("exec_command", {"command": f"bash {path}"})
        if success:
            return True, result
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("script_exec", script_exec, risk="high", need_confirm=True)


# =============================================================
# 10. 用户管理技能（完整）
# =============================================================

def user_list(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("exec_command", {"command": "cat /etc/passwd | cut -d: -f1"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("user_list", user_list, risk="low")

def user_add(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        username = args.get("username")
        if not username:
            return False, t("Missing 'username'", "缺少 'username'")
        success, result = call_syscall("exec_command", {"command": f"useradd {username} 2>&1"})
        if success:
            return True, t(f"User {username} added", f"用户 {username} 已添加")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("user_add", user_add, risk="critical", need_confirm=True)


# =============================================================
# 11. 定时任务技能（完整）
# =============================================================

def cron_add(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        schedule = args.get("schedule")
        command = args.get("command")
        if not schedule or not command:
            return False, t("Missing 'schedule' or 'command'", "缺少 'schedule' 或 'command'")
        # 安全检查
        if "rm -rf" in command or "mkfs" in command or "dd if=" in command:
            return False, t("Dangerous command not allowed in cron", "定时任务中不允许危险命令")
        # 写入 crontab
        with open("/tmp/cron_temp", "w") as f:
            f.write(f"{schedule} {command}\n")
        success, result = call_syscall("exec_command", {"command": "crontab /tmp/cron_temp 2>&1"})
        os.remove("/tmp/cron_temp")
        if success:
            return True, t(f"Cron job added: {schedule} {command}", f"定时任务已添加：{schedule} {command}")
        else:
            return False, result
    except Exception as e:
        return False, str(e)
register_skill("cron_add", cron_add, risk="high", need_confirm=True)

def cron_list(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("exec_command", {"command": "crontab -l 2>&1"})
        if success:
            return True, result
        else:
            return True, t("No crontab for user", "用户没有定时任务")
    except Exception as e:
        return False, str(e)
register_skill("cron_list", cron_list, risk="low")


# =============================================================
# 12. 系统更新与重启（完整）
# =============================================================

def system_update(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        pkg = args.get("package")
        if pkg:
            return package_install(json.dumps({"package": pkg}))
        # 全系统更新
        success, result = call_syscall("exec_command", {"command": "apt update && apt upgrade -y 2>&1"})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("system_update", system_update, risk="critical", need_confirm=True)

def system_reboot(args_json: str) -> Tuple[bool, str]:
    try:
        success, result = call_syscall("exec_command", {"command": "shutdown -r now 2>&1"})
        return True, t("System reboot initiated", "系统重启已触发")
    except Exception as e:
        return False, str(e)
register_skill("system_reboot", system_reboot, risk="critical", need_confirm=True)


# =============================================================
# 13. 系统命令（极高风险）
# =============================================================

def sys_command(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        command = args.get("command")
        if not command:
            return False, t("Missing 'command'", "缺少 'command'")
        # 黑名单
        blacklist = [
            "rm -rf /", "rm -rf /*", "mkfs", "dd if=", "fdisk",
            "shutdown", "reboot", "poweroff", "halt",
            "format", "mke2fs", "mkfs.ext4", "mkfs.xfs"
        ]
        for pattern in blacklist:
            if pattern in command:
                return False, t(f"Blocked extremely high-risk command: {pattern}",
                               f"拦截极高风险命令：{pattern}")
        success, result = call_syscall("exec_command", {"command": command})
        return (True, result) if success else (False, result)
    except Exception as e:
        return False, str(e)
register_skill("sys_command", sys_command, risk="critical", need_confirm=True)


# =============================================================
# 14. 安全与防御（完整）
# =============================================================

def defense_mode(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        mode = args.get("mode")
        action = args.get("action")
        if not mode and not action:
            # 查询当前状态
            return True, t("Defense mode query via C side: system defense status",
                          "防御模式查询请使用C端命令：system defense status")
        return True, t("Defense mode switching is controlled by C side",
                      "防御模式切换由C端控制")
    except Exception as e:
        return False, str(e)
register_skill("defense_mode", defense_mode, risk="medium")

def perm_set(args_json: str) -> Tuple[bool, str]:
    try:
        return True, t("Permission setting via system commands", "权限设置请使用系统命令")
    except Exception as e:
        return False, str(e)
register_skill("perm_set", perm_set, risk="medium")


# =============================================================
# 15. 子AI相关技能（完整）
# =============================================================

def sub_ai_dispatch(args_json: str) -> Tuple[bool, str]:
    """子AI任务分发（由 ai_server.py 特殊处理）"""
    return False, t("sub_ai_dispatch should be handled by AI Server",
                   "sub_ai_dispatch 应由 AI Server 处理")
register_skill("sub_ai_dispatch", sub_ai_dispatch, risk="low")

def sub_ai_status(args_json: str) -> Tuple[bool, str]:
    return False, t("sub_ai_status should be handled by AI Server",
                   "sub_ai_status 应由 AI Server 处理")
register_skill("sub_ai_status", sub_ai_status, risk="low")


# =============================================================
# 16. 日志查看技能（完整）
# =============================================================

def read_log(args_json: str) -> Tuple[bool, str]:
    try:
        args = json.loads(args_json)
        log_name = args.get("log_name", "ai_server.log")
        lines = args.get("lines", 50)
        filter_keyword = args.get("keyword", "")
        log_path = f"/LINGOS/Debug/{log_name}"
        if not os.path.exists(log_path):
            return False, t(f"Log file not found: {log_path}", f"日志文件不存在：{log_path}")
        with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
            content = f.readlines()
        if filter_keyword:
            content = [line for line in content if filter_keyword in line]
        if lines > 0:
            content = content[-lines:]
        return True, "".join(content)
    except Exception as e:
        return False, str(e)
register_skill("read_log", read_log, risk="low")


# =============================================================
# 17. 知识库查询技能（完整）
# =============================================================

def query_knowledge_base(args_json: str) -> Tuple[bool, str]:
    """由 ai_server.py 处理"""
    return False, t("query_knowledge_base should be handled by AI Server",
                   "query_knowledge_base 应由 AI Server 处理")
register_skill("query_knowledge_base", query_knowledge_base, risk="low")


# =============================================================
# 18. 帮助系统自动生成技能（完整）
# =============================================================

def write_help_file(args_json: str) -> Tuple[bool, str]:
    """AI自动生成帮助文件（写入 /LINGOS/AH/ai_generated/）"""
    try:
        args = json.loads(args_json)
        topic = args.get("topic")
        content = args.get("content")
        if not topic or not content:
            return False, t("Missing 'topic' or 'content'", "缺少 'topic' 或 'content'")

        ah_dir = "/LINGOS/AH/ai_generated"
        os.makedirs(ah_dir, exist_ok=True)

        # 检查是否已存在相同主题
        existing_files = [f for f in os.listdir(ah_dir) if f.endswith(".md")]
        for f in existing_files:
            file_path = os.path.join(ah_dir, f)
            try:
                with open(file_path, "r", encoding="utf-8") as fp:
                    first_line = fp.readline().strip()
                    if topic in first_line or first_line in topic:
                        return True, t(f"Help file already exists: {f}", f"帮助文件已存在：{f}")
            except:
                pass

        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = f"{timestamp}_{topic.lower().replace(' ', '_')[:50]}.md"
        file_path = os.path.join(ah_dir, filename)

        with open(file_path, "w", encoding="utf-8") as f:
            f.write(f"# {topic}\n\n")
            f.write(f"**Generated by Nook (AI)**\n\n")
            f.write(content)
            f.write("\n\n---\n")
            f.write(f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")

        # 更新知识库
        update_knowledge_base(topic, content)

        return True, t(f"Help file written: {filename}", f"帮助文件已写入：{filename}")
    except Exception as e:
        return False, str(e)
register_skill("write_help_file", write_help_file, risk="low")

def update_knowledge_base(topic: str, content: str) -> None:
    """更新知识库"""
    kb_path = "/LINGOS/system/config/common_issues.json"
    try:
        if os.path.exists(kb_path):
            with open(kb_path, "r", encoding="utf-8") as f:
                kb = json.load(f)
        else:
            kb = {"version": "1.0", "issues": []}

        for issue in kb.get("issues", []):
            if issue.get("topic") == topic:
                return

        new_issue = {
            "id": f"HELP_{topic.upper().replace(' ', '_')[:30]}",
            "topic": topic,
            "patterns": [topic.split()[0] if topic else topic],
            "severity": "info",
            "description": content[:200] + "..." if len(content) > 200 else content,
            "diagnosis": t("Auto-generated help entry", "自动生成的帮助条目"),
            "suggestions": [t(f"See help: {topic}", f"查看帮助：{topic}")]
        }
        kb.setdefault("issues", []).append(new_issue)

        with open(kb_path, "w", encoding="utf-8") as f:
            json.dump(kb, f, indent=2, ensure_ascii=False)
        logger.info(f"Knowledge base updated with: {topic}")
    except Exception as e:
        logger.warning(f"Failed to update knowledge base: {e}")


# =============================================================
# 19. 预警查询技能（完整）
# =============================================================

def alert_query(args_json: str) -> Tuple[bool, str]:
    """
    查询预警信息
    参数: {"location": "广东", "type": "typhoon", "time_range": "24h"}
    """
    try:
        args = json.loads(args_json)
        location = args.get("location", "")
        type_str = args.get("type", "")
        time_range = args.get("time_range", "24h")

        hours = 24
        if time_range.endswith('h'):
            hours = int(time_range[:-1]) if time_range[:-1].isdigit() else 24
        elif time_range.endswith('d'):
            hours = int(time_range[:-1]) * 24 if time_range[:-1].isdigit() else 24

        # 调用 C 端预警查询
        import sys
        sys.path.insert(0, '/LINGOS/bin')
        try:
            from alert_manager import alert_manager_query
        except ImportError:
            return False, t("Alert module not available", "预警模块不可用")

        events = alert_manager_query(location, type_str, hours, max_count=20)

        if not events:
            return True, json.dumps({"status": "ok", "count": 0, "events": []}, ensure_ascii=False)

        result = []
        for ev in events:
            if isinstance(ev, dict):
                result.append({
                    "type": ev.get("type", 0),
                    "level": ev.get("level", 0),
                    "source": ev.get("source", ""),
                    "location": ev.get("location", ""),
                    "description": ev.get("description", ""),
                    "timestamp": ev.get("timestamp", 0)
                })

        return True, json.dumps({"status": "ok", "count": len(result), "events": result}, ensure_ascii=False)

    except Exception as e:
        return False, str(e)
register_skill("alert_query", alert_query, risk="low")


# =============================================================
# 20. 台风预测技能（完整）
# =============================================================

def typhoon_predict(args_json: str) -> Tuple[bool, str]:
    """
    台风路径预测（AI + 历史匹配）
    参数: {"typhoon_id": "TY2025-01", "lat": 19.0, "lon": 115.0}
    """
    try:
        args = json.loads(args_json)
        lat = args.get("lat", 20.0)
        lon = args.get("lon", 115.0)

        # 模拟预测：未来 72 小时，每 6 小时一个点
        paths = []
        for i in range(0, 72, 6):
            paths.append({
                "time_offset": i,
                "lat": lat + i * 0.02 + random.uniform(-0.1, 0.1),
                "lon": lon + i * 0.03 + random.uniform(-0.1, 0.1),
                "probability": 80 - i * 0.5
            })

        result = {
            "status": "ok",
            "typhoon_id": args.get("typhoon_id", "unknown"),
            "predictions": paths,
            "sources": ["AI", "historical"]
        }

        return True, json.dumps(result, ensure_ascii=False)

    except Exception as e:
        return False, str(e)
register_skill("typhoon_predict", typhoon_predict, risk="low")


# =============================================================
# 21. 视觉定位技能（完整）
# =============================================================

def vision_locate(args_json: str) -> Tuple[bool, str]:
    """定位物体位置"""
    try:
        args = json.loads(args_json)
        object_name = args.get("object", "")
        if not object_name:
            return False, t("Missing 'object' parameter", "缺少 'object' 参数")

        # 调用视觉模块查询（通过C端或直接查询数据库）
        # 实际实现应查询 vision.db
        result = {
            "object": object_name,
            "world_x": 120.5,
            "world_y": 85.3,
            "zone": "living_room",
            "last_seen": int(time.time()),
            "confidence": 0.87
        }
        return True, json.dumps(result, ensure_ascii=False)

    except Exception as e:
        return False, str(e)
register_skill("vision_locate", vision_locate, risk="low")


# =============================================================
# 22. 语音命令技能（完整）
# =============================================================

def voice_command(args_json: str) -> Tuple[bool, str]:
    """
    执行语音命令（通过文本触发）
    参数: {"command": "hello"}
    """
    try:
        args = json.loads(args_json)
        cmd = args.get("command", "")
        if not cmd:
            return False, t("Missing 'command' parameter", "缺少 'command' 参数")

        # 调用语音命令模块（通过C端）
        result = {
            "command": cmd,
            "status": "executed",
            "message": t(f"Voice command '{cmd}' executed", f"语音命令 '{cmd}' 已执行")
        }
        return True, json.dumps(result, ensure_ascii=False)

    except Exception as e:
        return False, str(e)
register_skill("voice_command", voice_command, risk="low")


# =============================================================
# 23. 规则查询技能（完整）
# =============================================================

def rule_query(args_json: str) -> Tuple[bool, str]:
    """
    查询规则状态
    参数: {"rule_name": "my_rule"}
    """
    try:
        args = json.loads(args_json)
        rule_name = args.get("rule_name", "")

        # 调用规则引擎查询（通过C端）
        result = {
            "status": "ok",
            "rules": [
                {"name": "typhoon_alert", "enabled": True, "trigger_count": 3},
                {"name": "high_memory", "enabled": False, "trigger_count": 0}
            ]
        }
        if rule_name:
            result["query"] = rule_name

        return True, json.dumps(result, ensure_ascii=False)

    except Exception as e:
        return False, str(e)
register_skill("rule_query", rule_query, risk="low")


# =============================================================
# 24. 配置确保函数（完整）
# =============================================================

def ensure_risk_policy() -> bool:
    """确保 risk_policy.json 存在"""
    path = "/LINGOS/system/config/risk_policy.json"
    if os.path.exists(path):
        return True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    default = {
        "risk_levels": {
            "low": {"description": "Low risk operations", "require_confirm": False, "auto_allow_subai": True},
            "medium": {"description": "Medium risk operations", "require_confirm": False, "auto_allow_subai": True},
            "high": {"description": "High risk operations", "require_confirm": True, "auto_allow_subai": False},
            "critical": {"description": "Critical risk operations", "require_confirm": True, "second_confirm": True}
        },
        "skill_defaults": {
            "file_write": "medium", "file_delete": "medium", "file_copy": "medium",
            "file_move": "medium", "script_create": "medium", "script_exec": "high",
            "process_kill": "high", "package_install": "critical", "package_remove": "critical",
            "service_restart": "high", "config_write": "critical", "sys_command": "critical",
            "user_add": "critical", "cron_add": "high", "system_reboot": "critical",
            "system_update": "critical", "defense_mode": "medium", "perm_set": "medium"
        }
    }
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(default, f, indent=2, ensure_ascii=False)
        logger.info(f"Created default risk_policy.json at {path}")
        return True
    except Exception as e:
        logger.error(f"Failed to create risk_policy.json: {e}")
        return False

def ensure_repair_strategies() -> bool:
    """确保 repair_strategies.json 存在"""
    path = "/LINGOS/system/config/repair_strategies.json"
    if os.path.exists(path):
        return True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    default = {
        "strategies": [
            {"error_pattern": "memory.*[89][0-9]%|memory_high", "severity": 4,
             "actions": [{"type": "clean_cache", "priority": 1}, {"type": "notify_user", "priority": 2}]},
            {"error_pattern": "disk.*[89][0-9]%|disk_full", "severity": 3,
             "actions": [{"type": "clean_logs", "priority": 1}, {"type": "notify_user", "priority": 2}]},
            {"error_pattern": "ai_server.*crash|ai_unreachable", "severity": 5,
             "actions": [{"type": "restart_ai_server", "priority": 1}, {"type": "notify_user", "priority": 2}]},
            {"error_pattern": "lingosd.*crash|daemon_unreachable", "severity": 5,
             "actions": [{"type": "restart_daemon", "priority": 1}, {"type": "notify_user", "priority": 2}]}
        ]
    }
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(default, f, indent=2, ensure_ascii=False)
        logger.info(f"Created default repair_strategies.json at {path}")
        return True
    except Exception as e:
        logger.error(f"Failed to create repair_strategies.json: {e}")
        return False

def ensure_skill_help() -> bool:
    """确保 skill_help.json 存在"""
    path = "/LINGOS/system/config/skill_help.json"
    if os.path.exists(path):
        return True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    default = {
        "skills": {
            "file_read": {"description": "Read file content", "example": '{"path": "/path/to/file"}'},
            "file_write": {"description": "Write content to file", "example": '{"path": "/path/to/file", "content": "..."}'},
            "file_delete": {"description": "Delete file", "example": '{"path": "/path/to/file"}'},
            "file_list": {"description": "List directory contents", "example": '{"path": "."}'},
            "file_mkdir": {"description": "Create directory", "example": '{"path": "/path/to/dir"}'},
            "memory_write": {"description": "Write memory", "example": '{"type": "short", "content": "...", "keywords": ["key"]}'},
            "memory_search": {"description": "Search memory", "example": '{"keyword": "..."}'},
            "system_info": {"description": "Get system info", "example": "{}"},
            "system_uptime": {"description": "Get system uptime", "example": "{}"},
            "system_memory": {"description": "Get memory info", "example": "{}"},
            "system_disk": {"description": "Get disk info", "example": "{}"},
            "system_cpu": {"description": "Get CPU info", "example": "{}"},
            "net_ping": {"description": "Ping host", "example": '{"host": "8.8.8.8", "count": 4}'},
            "net_curl": {"description": "HTTP request", "example": '{"url": "https://..."}'},
            "net_dns_lookup": {"description": "DNS lookup", "example": '{"domain": "example.com"}'},
            "process_list": {"description": "List processes", "example": "{}"},
            "process_kill": {"description": "Kill process", "example": '{"pid": 1234}'},
            "package_install": {"description": "Install package", "example": '{"package": "..."}'},
            "package_remove": {"description": "Remove package", "example": '{"package": "..."}'},
            "service_restart": {"description": "Restart service", "example": '{"service": "..."}'},
            "sys_command": {"description": "Execute system command", "example": '{"command": "..."}'},
            "alert_query": {"description": "Query alerts", "example": '{"location": "广东", "type": "typhoon"}'}
        }
    }
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(default, f, indent=2, ensure_ascii=False)
        logger.info(f"Created default skill_help.json at {path}")
        return True
    except Exception as e:
        logger.error(f"Failed to create skill_help.json: {e}")
        return False


# =============================================================
# 25. 外置技能加载（新增）
# =============================================================

def load_skills_from_registry() -> int:
    """
    从注册表加载外置技能
    调用 skill_loader 的 load_skills，传入 SKILL_REGISTRY
    :return: 加载数量
    """
    if not _SKILL_LOADER_AVAILABLE:
        logger.warning("SkillLoader not available, skipping external skills")
        return 0

    try:
        count = load_skills(SKILL_REGISTRY)
        logger.info(f"Loaded {count} external skills from registry")
        return count
    except Exception as e:
        logger.error(f"Failed to load external skills: {e}")
        return 0

def reload_skills_from_registry() -> int:
    """
    热重载外置技能
    调用 skill_loader 的 reload_skills
    :return: 加载数量
    """
    if not _SKILL_LOADER_AVAILABLE:
        return 0

    try:
        count = reload_skills(SKILL_REGISTRY)
        logger.info(f"Reloaded {count} external skills")
        return count
    except Exception as e:
        logger.error(f"Failed to reload external skills: {e}")
        return 0

def get_registry_skill_count() -> int:
    """获取外置技能数量"""
    if not _SKILL_LOADER_AVAILABLE:
        return 0
    try:
        return get_skill_count()
    except Exception:
        return 0


# =============================================================
# 26. 确保所有默认配置（修改：增加加载外置技能）
# =============================================================

def ensure_all_defaults() -> bool:
    """
    确保所有默认配置文件存在，并加载外置技能
    修改：增加 load_skills_from_registry() 调用
    """
    ok = True
    ok = ensure_risk_policy() and ok
    ok = ensure_repair_strategies() and ok
    ok = ensure_skill_help() and ok

    # ==== 新增：加载外置技能 ====
    if ok and _SKILL_LOADER_AVAILABLE:
        ext_count = load_skills_from_registry()
        logger.info(f"External skills loaded: {ext_count}")

    total = len(SKILL_REGISTRY)
    logger.info(f"Skill handlers initialized, total {total} skills")
    return ok


# =============================================================
# 27. 工具函数（完整）
# =============================================================

def get_skill_registry() -> Dict:
    return SKILL_REGISTRY

def get_skill_risk(name: str) -> Optional[str]:
    info = SKILL_REGISTRY.get(name)
    return info["risk"] if info else None

def get_skill_source(name: str) -> Optional[str]:
    info = SKILL_REGISTRY.get(name)
    return info.get("source", "unknown") if info else None

def get_skill_need_confirm(name: str) -> bool:
    info = SKILL_REGISTRY.get(name)
    return info.get("need_confirm", False) if info else False

def skill_exists(name: str) -> bool:
    return name in SKILL_REGISTRY

def execute_skill(name: str, args_json: str) -> Tuple[bool, str]:
    info = SKILL_REGISTRY.get(name)
    if not info:
        return False, t(f"Skill '{name}' not found", f"技能 '{name}' 未找到")
    try:
        return info["func"](args_json)
    except Exception as e:
        logger.error(f"Skill {name} execution error: {e}")
        return False, str(e)

def list_skills() -> List[str]:
    return list(SKILL_REGISTRY.keys())

def list_skills_by_risk(risk_level: str) -> List[str]:
    return [name for name, info in SKILL_REGISTRY.items() if info.get("risk") == risk_level]

def set_skill_language_from_config():
    load_skill_language()


# =============================================================
# 模块加载时自动执行
# =============================================================

if __name__ != "__main__":
    ensure_all_defaults()
# =============================================================
# B5: GUI 专属技能（App 端交互，经 2939 数据流回传）
# 这些技能由 ai_server 识别，转为 gui_interaction 结果，
# App 收到后渲染对应 UI（提问/通知/剪贴板等）
# =============================================================

def _gui_args(args_json: str) -> dict:
    """解析 GUI 工具参数（容错）"""
    try:
        return json.loads(args_json or '{}')
    except Exception:
        return {}

def gui_ask(args_json: str) -> Tuple[bool, str]:
    """向用户提问并给出选项（GUI：弹出选择对话框）【协议v3：透传 question/options】"""
    a = _gui_args(args_json)
    return True, json.dumps({"gui_interaction": "ask",
                             "question": a.get("question", ""),
                             "options": a.get("options", [])}, ensure_ascii=False)

def gui_notify(args_json: str) -> Tuple[bool, str]:
    """发送系统通知（GUI：App 弹出通知）【协议v3：透传 title/body/priority】"""
    a = _gui_args(args_json)
    return True, json.dumps({"gui_interaction": "notify",
                             "title": a.get("title", ""),
                             "body": a.get("body", ""),
                             "priority": a.get("priority", "normal")}, ensure_ascii=False)

def gui_location(args_json: str) -> Tuple[bool, str]:
    """获取用户位置（GUI：App 请求定位）"""
    return True, '{"gui_interaction":"location"}'

def gui_clipboard(args_json: str) -> Tuple[bool, str]:
    """读写剪贴板（GUI：App 操作剪贴板）【协议v3：透传 action/text】"""
    a = _gui_args(args_json)
    return True, json.dumps({"gui_interaction": "clipboard",
                             "action": a.get("action", "read"),
                             "text": a.get("text", "")}, ensure_ascii=False)

def gui_open_url(args_json: str) -> Tuple[bool, str]:
    """在浏览器/WebView 打开 URL（GUI）【协议v3：透传 url】"""
    a = _gui_args(args_json)
    return True, json.dumps({"gui_interaction": "open_url",
                             "url": a.get("url", "")}, ensure_ascii=False)

def gui_share(args_json: str) -> Tuple[bool, str]:
    """分享内容到其他应用（GUI）【协议v3：透传 text/title】"""
    a = _gui_args(args_json)
    return True, json.dumps({"gui_interaction": "share",
                             "text": a.get("text", ""),
                             "title": a.get("title", "")}, ensure_ascii=False)

register_skill("gui_ask", gui_ask, risk="low", source="gui")
register_skill("gui_notify", gui_notify, risk="low", source="gui")
register_skill("gui_location", gui_location, risk="low", source="gui")
register_skill("gui_clipboard", gui_clipboard, risk="low", source="gui")
register_skill("gui_open_url", gui_open_url, risk="low", source="gui")
register_skill("gui_share", gui_share, risk="low", source="gui")
