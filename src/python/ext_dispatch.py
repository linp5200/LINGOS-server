#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 扩展命令统一分发（先生 2026-09-12 · 批次2~5 接入层）

把 home_ext / nvr_ext / ai_ext / ux_ext 的 60+ 命令统一映射到
ai_server 的命令表，避免逐个 if 分支（可维护性 + 可插件化）。

用法（在 ai_server 分发处）：
    from ext_dispatch import dispatch_ext
    if dispatch_ext(cmd, req, _reply, conn): return
"""

import logging

logger = logging.getLogger("ExtDispatch")

# =============================================================
# 命令 → (模块, 函数名) 映射
# =============================================================
EXT_MAP = {
    # ---------- 智能家居（批次2 · M1~M8）----------
    "discovery_scan":      ("home_ext", "cmd_discovery_scan"),
    "discovery_list":      ("home_ext", "cmd_discovery_list"),
    "discovery_set":       ("home_ext", "cmd_discovery_set"),
    "area_list":           ("home_ext", "cmd_area_list"),
    "area_add":            ("home_ext", "cmd_area_add"),
    "area_remove":         ("home_ext", "cmd_area_remove"),
    "label_list":          ("home_ext", "cmd_label_list"),
    "label_add":           ("home_ext", "cmd_label_add"),
    "entity_list":         ("home_ext", "cmd_entity_list"),
    "entity_add":          ("home_ext", "cmd_entity_add"),
    "entity_set_state":    ("home_ext", "cmd_entity_set_state"),
    "scene_list":          ("home_ext", "cmd_scene_list"),
    "scene_create":        ("home_ext", "cmd_scene_create"),
    "scene_apply":         ("home_ext", "cmd_scene_apply"),
    "energy_add":          ("home_ext", "cmd_energy_add"),
    "energy_summary":      ("home_ext", "cmd_energy_summary"),
    "energy_set_tariff":   ("home_ext", "cmd_energy_set_tariff"),
    "presence_get":        ("home_ext", "cmd_presence_get"),
    "presence_set":        ("home_ext", "cmd_presence_set"),
    "zone_list":           ("home_ext", "cmd_zone_list"),
    "zone_add":            ("home_ext", "cmd_zone_add"),
    "zone_remove":         ("home_ext", "cmd_zone_remove"),
    "zone_check":          ("home_ext", "cmd_zone_check"),
    "webhook_list":        ("home_ext", "cmd_webhook_list"),
    "webhook_add":         ("home_ext", "cmd_webhook_add"),
    "webhook_remove":      ("home_ext", "cmd_webhook_remove"),
    "webhook_trigger":     ("home_ext", "cmd_webhook_trigger"),
    "blueprint_list":      ("home_ext", "cmd_blueprint_list"),
    "blueprint_add":       ("home_ext", "cmd_blueprint_add"),
    "blueprint_apply":     ("home_ext", "cmd_blueprint_apply"),
    "automation_list":     ("home_ext", "cmd_automation_list"),
    "automation_add":      ("home_ext", "cmd_automation_add"),
    "automation_toggle":   ("home_ext", "cmd_automation_toggle"),
    "home_overview":       ("home_ext", "cmd_home_overview"),

    # ---------- 监控/NVR（批次3 · M9~M15）----------
    "motion_check":        ("nvr_ext", "motion_check"),
    "motion_config":       ("nvr_ext", "cmd_motion_config"),
    "motion_stats":        ("nvr_ext", "cmd_motion_stats"),
    "timeline_query":      ("nvr_ext", "cmd_timeline_query"),
    "timeline_segments":   ("nvr_ext", "cmd_timeline_segments"),
    "record_index_add":    ("nvr_ext", "cmd_record_index_add"),
    "record_mark_object":  ("nvr_ext", "cmd_record_mark_object"),
    "retention_config":    ("nvr_ext", "cmd_retention_config"),
    "storage_status":      ("nvr_ext", "cmd_storage_status"),
    "storage_cleanup":     ("nvr_ext", "cmd_storage_cleanup"),
    "storage_policy":      ("nvr_ext", "cmd_storage_policy"),
    "onvif_scan":          ("nvr_ext", "cmd_onvif_scan"),
    "onvif_list":          ("nvr_ext", "cmd_onvif_list"),
    "onvif_probe":         ("nvr_ext", "cmd_onvif_probe"),
    "restream_add":        ("nvr_ext", "cmd_restream_add"),
    "restream_list":       ("nvr_ext", "cmd_restream_list"),
    "restream_remove":     ("nvr_ext", "cmd_restream_remove"),
    "restream_attach":     ("nvr_ext", "cmd_restream_attach"),
    "restream_detach":     ("nvr_ext", "cmd_restream_detach"),
    "composite_list":      ("nvr_ext", "cmd_composite_list"),
    "composite_add":       ("nvr_ext", "cmd_composite_add"),
    "composite_remove":    ("nvr_ext", "cmd_composite_remove"),
    "composite_plan":      ("nvr_ext", "cmd_composite_render_plan"),
    "nvr_overview":        ("nvr_ext", "cmd_nvr_overview"),

    # ---------- AI（批次4 · M16~M19）----------
    "kb_upload":           ("ai_ext", "cmd_kb_upload"),
    "kb_list":             ("ai_ext", "cmd_kb_list"),
    "kb_remove":           ("ai_ext", "cmd_kb_remove"),
    "kb_search":           ("ai_ext", "cmd_kb_search"),
    "kb_ask":              ("ai_ext", "cmd_kb_ask"),
    "session_export":      ("ai_ext", "cmd_session_export"),
    "session_share_text":  ("ai_ext", "cmd_session_share_text"),
    "image_generate":      ("ai_ext", "cmd_image_generate"),
    "image_list":          ("ai_ext", "cmd_image_list"),

    # ---------- 组织/体验（批次5 · M20~M22）----------
    "notify_push":         ("ux_ext", "cmd_notify_push"),
    "notify_list":         ("ux_ext", "cmd_notify_list"),
    "notify_mark_read":    ("ux_ext", "cmd_notify_mark_read"),
    "notify_clear":        ("ux_ext", "cmd_notify_clear"),
    "notify_dnd":          ("ux_ext", "cmd_notify_dnd"),
    "dashboard_get":       ("ux_ext", "cmd_dashboard_get"),
    "dashboard_set":       ("ux_ext", "cmd_dashboard_set"),
    "dashboard_reset":     ("ux_ext", "cmd_dashboard_reset"),
    "dashboard_add_card":  ("ux_ext", "cmd_dashboard_add_card"),
    "dashboard_remove_card": ("ux_ext", "cmd_dashboard_remove_card"),
    "media_register":      ("ux_ext", "cmd_media_register"),
    "media_list":          ("ux_ext", "cmd_media_list"),
    "media_command":       ("ux_ext", "cmd_media_command"),
    "media_state":         ("ux_ext", "cmd_media_state"),
    "ux_overview":         ("ux_ext", "cmd_ux_overview"),
}

# 已加载的模块缓存
_MODS = {}


def _get_mod(name: str):
    if name in _MODS:
        return _MODS[name]
    try:
        import importlib
        m = importlib.import_module(name)
        _MODS[name] = m
        return m
    except Exception as e:
        logger.warning("load ext module '%s' failed: %s", name, e)
        _MODS[name] = None
        return None


def _build_kwargs(func, req: dict) -> dict:
    """按函数签名过滤参数（避免 unexpected keyword）"""
    try:
        import inspect
        sig = inspect.signature(func)
        allowed = set(sig.parameters.keys())
        kw = {k: v for k, v in (req or {}).items() if k in allowed}
        return kw
    except Exception:
        return {}


def is_ext_command(cmd: str) -> bool:
    return cmd in EXT_MAP


def dispatch_ext(cmd: str, req: dict, reply_fn, conn) -> bool:
    """统一分发扩展命令

    :param cmd:      收到的 cmd 名
    :param req:      完整请求体（含 params 展开）
    :param reply_fn: 形如 reply(conn, name, dict) 的回复函数
    :param conn:     连接对象
    :return: True 表示已处理（调用方应 return）
    """
    if cmd not in EXT_MAP:
        return False
    modname, fname = EXT_MAP[cmd]
    mod = _get_mod(modname)
    if mod is None:
        reply_fn(conn, cmd, {"status": "error", "error_type": "module_unavailable",
                             "msg": "扩展模块 %s 不可用" % modname})
        return True
    func = getattr(mod, fname, None)
    if func is None:
        reply_fn(conn, cmd, {"status": "error", "error_type": "not_implemented",
                             "msg": "%s.%s 未实现" % (modname, fname)})
        return True
    try:
        kw = _build_kwargs(func, req)
        # 位置参数兜底（部分函数签名要求必填位置参数）
        import inspect
        sig = inspect.signature(func)
        required = [p for p in sig.parameters.values()
                    if p.default is inspect.Parameter.empty
                    and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
                    and p.name not in kw]
        args = []
        for p in required:
            args.append(req.get(p.name, None))
        result = func(*args, **kw)
        if not isinstance(result, dict):
            result = {"status": "ok", "data": result}
        reply_fn(conn, cmd, result)
    except Exception as e:
        logger.exception("ext command '%s' failed", cmd)
        reply_fn(conn, cmd, {"status": "error", "error_type": "execution_error",
                             "msg": str(e)})
    return True


def ext_command_list():
    """返回全部扩展命令名（供 App/Web 发现）"""
    return sorted(EXT_MAP.keys())
