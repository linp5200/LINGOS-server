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
    # ---------------- 【2026-10-11 P2 ②】36 个无闸门技能补齐 ----------------
    # 原则：只读技能 → 读权限（默认放行，用户可在 App 收紧到 deny）；
    #      资源域技能 → 对应域权限；生命线读 → 独立权限（不被隐私设置误伤）。
    # 文件/日志/配置读（只读；映射 UI external_storage）
    "file_read":         "file_read",     # 读文件内容
    "file_list":         "file_read",     # 目录列举
    "read_log":          "file_read",     # 读 /LINGOS/Debug/*.log
    "system_log":        "file_read",     # 读 /var/log/syslog（经 syscall file_read）
    "config_read":       "file_read",     # 读配置（只读）
    "rule_query":        "file_read",     # 读 /LINGOS/system/config/rules.json
    # 记忆/知识库（只读）
    "memory_read":       "memory_read",
    "memory_search":     "memory_read",
    "memory_index":      "memory_read",
    "query_knowledge_base": "memory_read", # 知识库检索（ai_server 处理）
    "vision_locate":     "memory_read",   # 读视觉记忆库 vision.db——不是开摄像头
    # 仓库只读
    "git_status":        "git_read",
    "git_log":           "git_read",
    "git_diff":          "git_read",
    # 系统度量/进程/服务（只读）
    "system_info":       "system_query",
    "system_cpu":        "system_query",
    "system_memory":     "system_query",
    "system_disk":       "system_query",
    "system_uptime":     "system_query",
    "process_list":      "system_query",
    "process_info":      "system_query",
    "service_status":    "system_query",
    "user_list":         "system_query",   # 经 exec_command 读 /etc/passwd（只读效果）
    # 应用/包查询（映射 UI installed_apps）
    "package_list":      "package_query",
    "package_search":    "package_query",
    # 网络域
    "net_dns_lookup":    "network_access",
    "typhoon_predict":   "network_access", # 国家气象中心 HTTP 数据源
    "gui_open_url":      "network_access", # 在 App 打开外部 URL
    # 定时任务域（cron_manage 此前在默认放行集合中但不可达）
    "cron_list":         "cron_manage",
    # 家居域只读（映射 UI ha_control）
    "ha_search":         "ha_read",
    # 用户交互（无资源访问）
    "gui_ask":           "ui_interaction",
    "gui_notify":        "ui_interaction",
    # AI 内部委派（ai_server 处理）
    "sub_ai_dispatch":   "sub_ai",
    "sub_ai_status":     "sub_ai",
    # 生命线读（不映射任何 UI 权限——警报/预警不得被隐私设置误伤）
    "alert_query":       "alert_read",
    # 写类（复用既有权限名）
    "write_help_file":   "file_write",
}


def _t(en: str, zh: str) -> str:
    """双语（与 skill_handlers 保持一致）"""
    try:
        from skill_handlers import t as _sht
        return _sht(en, zh)
    except Exception:
        return zh


# =============================================================
# 【2026-10-11 P2 ③】权限存储读取（缺失/损坏 → fail-safe + 明确日志，不再静默）
# =============================================================
_PERM_STORE_PATH = "/LINGOS/system/config/ai_permissions.json"
_store_warned = False


def _load_perm_store() -> dict:
    """读 /LINGOS/system/config/ai_permissions.json

    缺失/不可解析 → 返回 {}，并**一次性 WARNING**（fail-safe 由调用方按域默认执行：
    操作类放行[审计]、隐私类拒绝）。返回 {} 与"用户什么都没设置"语义一致。
    """
    global _store_warned
    try:
        if not os.path.exists(_PERM_STORE_PATH):
            if not _store_warned:
                logger.warning(
                    "ai_permissions.json 缺失（%s）→ fail-safe：操作类按域默认放行[审计]、"
                    "隐私类拒绝；App「设置→权限」写入后即刻生效", _PERM_STORE_PATH)
                _store_warned = True
            return {}
        with open(_PERM_STORE_PATH, "r", encoding="utf-8") as _f:
            _data = json.load(_f)
        if not isinstance(_data, dict):
            if not _store_warned:
                logger.warning("ai_permissions.json 顶层非对象 → 忽略（%s）", _PERM_STORE_PATH)
                _store_warned = True
            return {}
        return _data
    except Exception as e:
        if not _store_warned:
            logger.warning("ai_permissions.json 读取失败（%s）：%s → fail-safe 同上",
                           _PERM_STORE_PATH, e)
            _store_warned = True
        return {}


def _query_permission_mode(perm_name: str):
    """查询权限的原始模式：'deny' | 'shadow' | 'allow' | None（未知）

    【0.6.0】影子模式支持（三态：deny / shadow / allow）
      · deny   → 拒绝执行
      · shadow → 执行被拦截，返回"结构正确但内容为空"的假成功（不报错、不泄露）
      · allow  → 正常执行
    """
    if not perm_name:
        return "allow"
    _store = _load_perm_store()
    if _store:
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
    # 存储不可用（缺失/损坏）→ 域默认（fail-safe：操作类放行、隐私类拒绝）
    #   缺失告警已在 _load_perm_store() 内一次性打印（不再静默）
    if perm_name in _DEFAULT_ALLOW_PERMS:
        return "allow"
    return "deny"


# =============================================================
# 【2026-10-10 P3】影子模式两层统一 · 一期（零 C 改动）
# -------------------------------------------------------------
# 背景：C 端 defense_mode（有效，但执行点仅 exec_command）与 Python 网关三态
#   （App「设置→权限」写入 ai_permissions.json）此前**互不相通**：
#   用户在 App 开影子不影响 C 端；shell 里 system defense shadow on 也不影响网关。
# 先生裁决（2026-10-10）：**最严优先合成** —— 任一侧影子 → 判定为影子。
# C 端已通过 syscall permission_list 上报 defense_mode / shadow_active
#   （src/daemon/syscall_handler.c:1480-1482），本模块读取并合成。
# 二期（待做）：新增 defense_mode_set syscall，让 App 一处控制两层。
# =============================================================
_c_defense_cache = {"ts": 0.0, "defense_mode": None, "shadow_active": False, "ok": False}
_C_DEFENSE_TTL = 5.0


def _c_defense_state(ttl: float = _C_DEFENSE_TTL) -> dict:
    """查询 C 端防御模式状态（TTL 缓存——避免每次技能调用都走 daemon.sock）"""
    import time as _time
    now = _time.time()
    if _c_defense_cache["ok"] and (now - _c_defense_cache["ts"]) < ttl:
        return _c_defense_cache
    try:
        import sys as _sys, os as _os
        _gdir = _os.path.dirname(_os.path.abspath(__file__))
        if _gdir not in _sys.path:
            _sys.path.insert(0, _gdir)
        from syscall_client import call_syscall
        ok, res = call_syscall("permission_list", {}, timeout=5)
        if ok:
            data = json.loads(res) if isinstance(res, str) else res
            inner = data.get("data", data) if isinstance(data, dict) else {}
            if isinstance(inner, str):
                inner = json.loads(inner or "{}")
            if isinstance(inner, dict):
                _c_defense_cache.update({
                    "ts": now,
                    "defense_mode": inner.get("defense_mode"),
                    "shadow_active": bool(inner.get("shadow_active")),
                    "ok": True,
                })
                logger.debug("C defense state: mode=%s shadow=%s",
                             _c_defense_cache["defense_mode"], _c_defense_cache["shadow_active"])
    except Exception as e:
        logger.debug("C defense state query failed: %s", e)
    return _c_defense_cache


def c_shadow_active() -> bool:
    """C 端当前是否处于影子模式（供最严优先合成）"""
    return bool(_c_defense_state().get("shadow_active"))


def is_shadow_skill(skill_name: str, risk: str = "low") -> bool:
    """该技能是否处于影子模式（执行被拦截→返回结构正确的空数据）

    影子模式语义（先生定稿）：功能"看起来在工作"但不接触真实数据——
    隐私保护与演示/审计场景用。

    【2026-10-10 P3】**两层最严优先合成**：
      ① Python 侧（App 权限设置，按权限粒度）判 shadow → 影子
      ② C 端 defense_mode=shadow（全局）→ 有权限映射的技能一律影子
    两处任一为真即影子；来源写入 debug 日志便于追查。
    """
    perm = SKILL_PERM_OVERRIDE.get(skill_name)
    if perm is None:
        perm = RISK_PERM_MAP.get(risk or "low")
    if not perm:
        # 无权限映射（无副作用技能）→ 不受影子影响（保持原判语义）
        return False

    py_shadow = False
    mode = _query_permission_mode(perm)
    if mode == "shadow":
        py_shadow = True
    # UI 映射权限（如 location_access → 用户可能只给 location 设了 shadow）
    elif perm in _UI_PERM_MAP and _query_permission_mode(_UI_PERM_MAP[perm]) == "shadow":
        py_shadow = True

    if py_shadow:
        return True
    # C 端全局影子（此前对 Python 技能完全不可见——"影子做成谎言"的根因）
    if c_shadow_active():
        logger.debug("shadow via C defense_mode for skill '%s' (perm=%s)", skill_name, perm)
        return True
    return False


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
    _store = _load_perm_store()
    if _store:
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
    # 存储缺失/损坏 → 明确日志已在 _load_perm_store() 内打印；继续走 daemon 兜底

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

# =============================================================
# 【2026-10-11 P2 ①】App 用户权限 18 项注册表（此前仅 6 项有映射）
# -------------------------------------------------------------
# 事实源：LINGOS-APP/lib/features/ai_config/screens/permission_screen.dart
#   （kPermLabels 17 键 + ha_control；历史文档称"19 项"系数错，实为 18）
# 目的：App「设置→权限」每一项都要**有落点**——否则用户设为 deny/shadow
#   不产生任何效果（改前 16 项 0 覆盖）。落点 = 技能闸门 或 命令面闸门。
# =============================================================
USER_PERMS = (
    "location", "camera", "record_audio", "record_screen", "accelerometer",
    "phone_state", "installed_apps", "external_storage", "network_control",
    "bluetooth_control", "scan_bluetooth", "launch_app", "install_app",
    "jump_app", "background_data", "background_task", "auto_start", "ha_control",
)

# 用户权限 → 网关权限名（落点）；预留位=执行点在 App/命令面（技能面暂无技能使用）
USER_PERM_TO_GATEWAY = {
    "location":          "location_access",   # 技能 gui_location + App 定位请求
    "camera":            "camera_access",     # 命令面 camera_snapshot/monitor_snapshot/record_start/ai_vision_detect
    "record_audio":      "audio_access",      # 语音命令面/voiced（C 侧 permission_check_device）
    "record_screen":     "screen_access",     # 预留（尚无截屏能力）
    "accelerometer":     "sensor_access",     # 预留（App 侧传感器）
    "phone_state":       "phone_state",       # 预留（App 侧查询）
    "installed_apps":    "package_query",     # 技能 package_list / package_search
    "external_storage":  "file_read",         # 技能 file_read/file_list/read_log（C 侧另有 file_check）
    "network_control":   "network_access",    # 技能 net_*/web_fetch/web_search/dns/typhoon_predict
    "bluetooth_control": "bluetooth_access",  # 预留（C 侧 PERM_BLUETOOTH 已就绪）
    "scan_bluetooth":    "bluetooth_scan",    # 预留
    "launch_app":        "app_launch",        # 预留
    "install_app":       "package_manage",    # 技能 package_install / package_remove
    "jump_app":          "app_jump",          # 预留
    "background_data":   "background_data",   # 预留
    "background_task":   "background_task",   # 预留
    "auto_start":        "auto_start",        # 预留
    "ha_control":        "ha_read",           # 技能 ha_search（读）；控制侧由 ai_server ha_control 命令面把关
}

# 【0.6.0】网关权限名 → App UI 权限名映射（用户设置联动）
#   【2026-10-11 P2 ①】补齐 18 项落点：任一网关权限被用户收紧 → 对应用户权限即生效
_UI_PERM_MAP = {
    # 设备/隐私（未授予=拒绝——沿用 0.6.0 隐私类默认）
    "location_access":  "location",
    "camera_access":    "camera",
    "audio_access":     "record_audio",
    "screen_access":    "record_screen",
    "bluetooth_access": "bluetooth_control",
    # 网络/存储/应用/家居（默认放行，用户可在 App 收紧到 deny）
    "network_access":   "network_control",
    "file_read":        "external_storage",
    "package_query":    "installed_apps",
    "package_manage":   "install_app",
    "ha_read":          "ha_control",
    # 预留位（执行点待接线；登记以保证 App 端设置**立刻对同名命令生效**）
    "sensor_access":    "accelerometer",
    "bluetooth_scan":   "scan_bluetooth",
    "app_launch":       "launch_app",
    "app_jump":         "jump_app",
    "background_data":  "background_data",
    "background_task":  "background_task",
    "auto_start":       "auto_start",
    "phone_state":      "phone_state",
}

# 【0.6.0】操作类权限默认放行（受风险分级 + 审批链 + 审计三重约束——
#   用户可在权限存储中显式置 deny 收紧）
_DEFAULT_ALLOW_PERMS = {
    "sys_command", "file_write", "file_delete", "package_manage",
    "service_manage", "power_control", "system_update", "user_manage",
    "security_config", "permission_admin", "voice_control",
    "clipboard_access", "share_access", "network_access",
    "cron_manage", "script_exec",
    # 【2026-10-11 P2 ②】只读/查询类（36 个无闸门技能补齐后引入的读权限名）
    #   默认放行 = 不改既有可用性（铁律：不减少功能）；用户收紧对应 UI 权限即生效
    "file_read", "system_query", "memory_read", "git_read", "package_query",
    "ui_interaction", "alert_read", "ha_read", "sub_ai",
    # 预留位（保证 App 未设置时不误拒；设置 deny 即拒绝）
    "sensor_access", "bluetooth_scan", "app_launch", "app_jump",
    "background_data", "background_task", "auto_start", "phone_state",
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


# =============================================================
# 【2026-10-11 P2 ①】命令面闸门（技能之外的第二类执行点）
# -------------------------------------------------------------
# 用于 camera 等"直接触发资源采集"的命令（此前 camera 在 App 可设，但命令面
# 无任何落点 → 用户设 deny/shadow 不产生效果）。
# =============================================================

def user_perm_coverage() -> dict:
    """自检：18 项用户权限 → 网关落点（供审计/诊断调用）

    返回 {用户权限: 网关权限名 or None}；None = 执行点在 App/命令面（技能面暂无使用）。
    """
    return {p: USER_PERM_TO_GATEWAY.get(p) for p in USER_PERMS}


def command_perm_gate(ui_perm: str) -> dict:
    """命令面（HTTP 8088 / daemon 命令）权限闸门

    返回 {"decision": "allow"|"deny"|"shadow", "message": str}
      · 显式 deny   → deny（调用方拒绝并给出明确原因）
      · 显式 shadow → shadow（调用方返回结构正确的空数据，不接触真实资源）
      · 其他/未设置 → allow（**默认放行**：命令面调用者可能是用户本人，
        避免"未设置即禁用"使监控/视觉默认不可用）
      · 存储不可读 → allow + 明确日志（命令面不因权限存储缺失而瘫痪）

    ⚠️ 口径说明：App 未设置项在 UI 上显示为"拒绝"，而命令面未设置=放行——
    两者差异已记录待先生裁定（改为默认拒绝需先生确认，涉及监控可用性）。
    """
    if not ui_perm or ui_perm not in USER_PERMS:
        return {"decision": "allow", "message": ""}
    try:
        _store = _load_perm_store()
        _mode = _store.get(ui_perm) if _store else None
        if _mode == "deny":
            logger.warning("command-face perm '%s' = deny → refused", ui_perm)
            return {"decision": "deny", "message": _t(
                f"Permission '{ui_perm}' is DISABLED. Enable it in Settings → Permissions.",
                f"权限 '{ui_perm}' 当前为**禁用**状态。请在「设置 → 权限」中开启后重试。")}
        if _mode == "shadow":
            logger.info("command-face perm '%s' = shadow → empty result", ui_perm)
            return {"decision": "shadow", "message": _t("Request processed.", "请求已处理。")}
    except Exception as e:
        logger.warning("command-face perm gate error (%s) → allow (fail-safe, logged)", e)
    return {"decision": "allow", "message": ""}
