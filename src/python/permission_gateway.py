#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 权限网关（先生 2026-09-12 裁决 S17）

背景（审计发现）：
    Python 侧的技能执行**完全没有权限校验** → 权限系统（19 权限 × 5 模式）
    对 AI 调用形同虚设。AI 可绕过权限直接调用任意技能。

设计依据（OWASP LLM06 §7「Complete mediation」）：
    授权必须在**底层系统**完成，不能依赖 LLM 自行判断。

本模块：
    · 技能执行前的**统一权限闸门**（guard）
    · 按技能风险等级 + 目标资源判定
    · 高危技能未授权 → 拒绝并给出明确原因（不静默放行）
    · 环境不可用时**安全默认**：高危拒绝、低危放行（避免系统不可用）

与 C 端的关系：
    文件路径类 → 交 daemon 的 permission_check_file（已实现，含 realpath）
    通用技能类 → 本模块按 risk 等级 + 权限表判定
"""

import json
import logging
from typing import Tuple

logger = logging.getLogger("PermGateway")

# =============================================================
# 风险等级 → 所需权限名（与 C 端 AI_PERMS 对齐）
# =============================================================
RISK_PERM_MAP = {
    # 低危：无副作用或只读
    "low": None,
    # 中危：写文件/改状态/网络请求
    "medium": "file_write",
    # 高危：执行命令/改动系统
    "high": "sys_command",
    # 严重：不可逆操作
    "critical": "sys_command",
}

# 按技能名的显式覆盖（比 risk 更精确）
SKILL_PERM_OVERRIDE = {
    # 系统操作
    "sys_command":        "sys_command",
    "script_exec":        "sys_command",
    "package_install":    "package_manage",
    "package_remove":     "package_manage",
    "service_restart":    "service_manage",
    "system_reboot":      "power_control",
    "system_update":      "system_update",
    "cron_add":           "sys_command",
    "user_add":           "user_manage",
    "defense_mode":       "security_config",
    "perm_set":           "permission_admin",
    # 文件
    "file_write":         "file_write",
    "file_delete":        "file_delete",
    "file_move":          "file_write",
    "file_copy":          "file_write",
    "file_mkdir":         "file_write",
    # 网络
    "net_curl":           "network_access",
    "web_fetch":          "network_access",
    "web_search":         "network_access",
    "net_ping":           "network_access",
    # 设备/隐私
    "gui_location":       "location_access",
    "gui_clipboard":      "clipboard_access",
    "gui_share":          "share_access",
    "voice_command":      "voice_control",
}


def _t(en: str, zh: str) -> str:
    """双语（与 skill_handlers 保持一致）"""
    try:
        from skill_handlers import t as _sht
        return _sht(en, zh)
    except Exception:
        return zh


def _query_permission(perm_name: str) -> int:
    """查询权限状态：1=允许 0=禁止 -1=未知/不可用

    优先走 daemon（权限系统的权威判定）；不可用时返回 -1（交由调用方按安全默认处理）。
    """
    if not perm_name:
        return 1
    try:
        from syscall_client import call_syscall
        ok, res = call_syscall("permission_list", {}, timeout=5)
        if not ok:
            return -1
        data = res
        if isinstance(res, str):
            try:
                data = json.loads(res)
            except Exception:
                return -1
        perms = None
        if isinstance(data, dict):
            inner = data.get("data", data)
            if isinstance(inner, str):
                try:
                    inner = json.loads(inner)
                except Exception:
                    inner = {}
            if isinstance(inner, dict):
                perms = inner.get("permissions") or inner.get("perms") or inner.get("list")
        if isinstance(perms, list):
            for p in perms:
                if isinstance(p, dict):
                    nm = p.get("name") or p.get("id") or p.get("key")
                    if nm == perm_name:
                        v = p.get("allowed", p.get("enabled", p.get("value")))
                        if isinstance(v, bool):
                            return 1 if v else 0
                        if isinstance(v, (int, float)):
                            return 1 if v else 0
                        if isinstance(v, str):
                            return 1 if v.lower() in ("1", "true", "allow", "on", "yes") else 0
        return -1
    except Exception as e:
        logger.debug("perm query failed: %s", e)
        return -1


def check_skill_permission(skill_name: str, risk: str = "low",
                           args: dict = None) -> Tuple[bool, str]:
    """技能执行前的权限闸门

    :return: (allowed, reason)。allowed=False 时 reason 为给 AI 的说明
    """
    # ① 确定所需权限
    perm = SKILL_PERM_OVERRIDE.get(skill_name)
    if perm is None:
        perm = RISK_PERM_MAP.get(risk or "low")

    # 低危且无映射 → 放行（无副作用）
    if not perm:
        return True, ""

    # ② 询问权限系统
    st = _query_permission(perm)

    if st == 1:
        return True, ""
    if st == 0:
        logger.warning("skill '%s' denied: permission '%s' disabled", skill_name, perm)
        return False, _t(
            f"Blocked by permission system: skill '{skill_name}' requires permission "
            f"'{perm}' which is currently DISABLED. Ask the user to enable it in "
            f"Settings → Permissions.",
            f"已被权限系统拒绝：技能 '{skill_name}' 需要权限 '{perm}'，当前为**禁用**状态。"
            f"请在「设置 → 权限」中开启后重试。",
        )

    # ③ 权限系统不可用（st == -1）→ 安全默认
    #    高危/严重：拒绝（宁可不可用，也不越权）
    #    中危：允许但记录警告（避免系统整体不可用）
    if (risk or "low") in ("high", "critical"):
        logger.warning("perm service unavailable → deny high-risk skill '%s'", skill_name)
        return False, _t(
            f"Permission service unavailable — high-risk skill '{skill_name}' DENIED "
            f"(fail-safe). Please retry after the daemon is available.",
            f"权限服务不可用 —— 高危技能 '{skill_name}' 已按**安全默认**拒绝。"
            f"请确认守护进程运行后重试。",
        )
    logger.warning("perm service unavailable → allow medium-risk skill '%s' (degraded)", skill_name)
    return True, ""


def get_skill_risk(skill_name: str) -> str:
    """取技能风险等级（供审计/日志）"""
    try:
        from skill_handlers import SKILL_REGISTRY
        info = SKILL_REGISTRY.get(skill_name)
        if info:
            return info.get("risk", "low")
    except Exception:
        pass
    try:
        import os as _os, sys as _sys
        _pdir = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "plugin")
        if _os.path.isdir(_pdir) and _pdir not in _sys.path:
            _sys.path.insert(0, _pdir)
        from plugin_loader import get_loader
        sk = getattr(get_loader(), "_skills", {}) or {}
        if skill_name in sk:
            return sk[skill_name].get("risk", "low")
    except Exception:
        pass
    return "low"
