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
import os
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


def _query_permission_mode(perm_name: str):
    """查询权限的原始模式：'deny' | 'shadow' | 'allow' | None（未知）

    【0.6.0】影子模式支持（三态：deny / shadow / allow）
      · deny   → 拒绝执行
      · shadow → 执行被拦截，返回"结构正确但内容为空"的假成功（不报错、不泄露）
      · allow  → 正常执行
    """
    if not perm_name:
        return "allow"
    try:
        _pfile = "/LINGOS/system/config/ai_permissions.json"
        if os.path.exists(_pfile):
            with open(_pfile, "r", encoding="utf-8") as f:
                _store = json.load(f)
            if isinstance(_store, dict):
                _mode = _store.get(perm_name)
                if _mode is None:
                    _ui = _UI_PERM_MAP.get(perm_name)
                    if _ui is not None:
                        _mode = _store.get(_ui)
                if _mode is not None:
                    if _mode == "deny":
                        return "deny"
                    if _mode == "shadow":
                        return "shadow"
                    return "allow"
                # 未显式配置 → 域默认
                if perm_name in _DEFAULT_ALLOW_PERMS:
                    return "allow"
                return "allow" if _privacy_default(perm_name, _store) == 1 else "deny"
    except Exception as e:
        logger.debug("perm mode read failed: %s", e)
    return None


def is_shadow_skill(skill_name: str, risk: str = "low") -> bool:
    """【0.6.0】该技能是否处于影子模式（执行被拦截→返回空数据）

    影子模式语义（先生定稿）：功能"看起来在工作"但不接触真实数据——
    隐私保护与演示/审计场景用。判定顺序与 check_skill_permission 一致。
    """
    perm = SKILL_PERM_OVERRIDE.get(skill_name)
    if perm is None:
        perm = RISK_PERM_MAP.get(risk or "low")
    if not perm:
        return False
    mode = _query_permission_mode(perm)
    # UI 映射权限（如 location_access → 用户可能只给 location 设了 shadow）
    if mode != "shadow" and perm in _UI_PERM_MAP:
        mode2 = _query_permission_mode(_UI_PERM_MAP[perm])
        if mode2 == "shadow":
            return True
    return mode == "shadow"


def build_shadow_result(skill_name: str, args: dict = None) -> str:
    """【0.6.0】构造影子模式的假成功结果（结构正确、内容空）

    与真实结果同构（status/ok 字段齐全）——AI 与 UI 无感知差异，
    但不接触任何真实数据。
    """
    empty = {
        "status": "ok",
        "data": [],
        "count": 0,
        "shadow": True,
        "message": _t("Request processed.", "请求已处理。"),
    }
    return json.dumps(empty, ensure_ascii=False)


def _query_permission(perm_name: str) -> int:
    """查询权限状态：1=允许 0=禁止 -1=未知/不可用

    【0.6.0 重写】修复「权限服务恒不可用 → 11 高危技能永久死锁」：
      ① 直读 AI 权限存储 /LINGOS/system/config/ai_permissions.json
         （与 App「设置→权限」同一事实源——用户改了立即生效）
      ② 未配置项按域默认：操作类放行（受风险分级+审批+审计约束），
         隐私类映射到 UI 权限（未授予=拒绝）
      ③ daemon permission_list 兜底（保留兼容）
    """
    if not perm_name:
        return 1

    # ---- ① 直读权限存储（首选） ----
    try:
        _pfile = "/LINGOS/system/config/ai_permissions.json"
        if os.path.exists(_pfile):
            with open(_pfile, "r", encoding="utf-8") as f:
                _store = json.load(f)
            if isinstance(_store, dict):
                _mode = _store.get(perm_name)
                if _mode is None:
                    # UI 可映射权限 → 跟随 UI 设置
                    _ui = _UI_PERM_MAP.get(perm_name)
                    if _ui is not None:
                        _mode = _store.get(_ui)
                if _mode is not None:
                    if _mode == "deny":
                        return 0
                    # allow_once / allow_while / allow_always / shadow → 放行
                    return 1
                # 未显式配置 → 域默认
                return 1 if perm_name in _DEFAULT_ALLOW_PERMS else _privacy_default(perm_name, _store)
    except Exception as e:
        logger.debug("perm store read failed: %s", e)

    # ---- ② daemon 兜底（旧路径保留） ----
    try:
        from syscall_client import call_syscall
        ok, res = call_syscall("permission_list", {}, timeout=5)
        if ok:
            data = res
            if isinstance(res, str):
                try:
                    data = json.loads(res)
                except Exception:
                    data = None
            if isinstance(data, dict):
                inner = data.get("data", data)
                if isinstance(inner, str):
                    try:
                        inner = json.loads(inner)
                    except Exception:
                        inner = {}
                perms = inner.get("permissions") or inner.get("perms") or inner.get("current") \
                    if isinstance(inner, dict) else None
                if isinstance(perms, dict):
                    v = perms.get(perm_name)
                    if v is not None:
                        return 0 if v == "deny" else 1
                elif isinstance(perms, list):
                    for p in perms:
                        if isinstance(p, dict):
                            nm = p.get("name") or p.get("id") or p.get("key")
                            if nm == perm_name:
                                v = p.get("mode", p.get("allowed", p.get("enabled")))
                                if isinstance(v, bool):
                                    return 1 if v else 0
                                if isinstance(v, str):
                                    return 0 if v == "deny" else 1
    except Exception as e:
        logger.debug("perm daemon query failed: %s", e)

    # ---- ③ 都不可用 → 安全默认（操作类放行[审计]，隐私类拒绝） ----
    if perm_name in _DEFAULT_ALLOW_PERMS:
        logger.info("perm '%s' store unavailable → allow by domain default (audited)", perm_name)
        return 1
    return -1

# 【0.6.0】网关权限名 → App UI 权限名映射（用户设置联动）
_UI_PERM_MAP = {
    "location_access": "location",
    "camera_access": "camera",
    "audio_access": "record_audio",
    "screen_access": "record_screen",
    "network_access": "network_control",
    "bluetooth_access": "bluetooth_control",
}

# 【0.6.0】操作类权限默认放行（受风险分级 + 审批链 + 审计三重约束——
#   用户可在权限存储中显式置 deny 收紧）
_DEFAULT_ALLOW_PERMS = {
    "sys_command", "file_write", "file_delete", "package_manage",
    "service_manage", "power_control", "system_update", "user_manage",
    "security_config", "permission_admin", "voice_control",
    "clipboard_access", "share_access", "network_access",
    "cron_manage", "script_exec",
}

def _privacy_default(perm_name: str, store: dict) -> int:
    """隐私类默认：映射 UI 权限时跟随（未授予=拒绝）；无映射=拒绝"""
    _ui = _UI_PERM_MAP.get(perm_name)
    if _ui:
        _m = store.get(_ui)
        if _m is None:
            return 0  # 未授予 → 拒绝（与 App 显示一致）
        return 0 if _m == "deny" else 1
    return 0


def check_skill_permission(skill_name: str, risk: str = "low",
                           args: dict = None) -> Tuple[bool, str]:
    """技能执行前的权限闸门

    :return: (allowed, reason)。allowed=False 时 reason 为给 AI 的说明
    """
    # 【0.7.0 P2.5 B4】危机全权模式（§2B 铁律：危险时刻全权响应——audit-only）
    #   危机进行中：跳过一切权限限制（含平时禁用的高危项），仅记录审计日志。
    #   危机解除后自动回归常规（仅 crisis_active() 为真时生效）。
    try:
        import sys as _sys, os as _os
        _gdir = _os.path.dirname(_os.path.abspath(__file__))
        if _gdir not in _sys.path:
            _sys.path.insert(0, _gdir)
        from crisis import crisis_active as _crisis_on
        if _crisis_on():
            logger.warning("CRISIS full-authority: skill '%s' allowed (audit-only)", skill_name)
            return True, ""
    except Exception:
        pass

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
