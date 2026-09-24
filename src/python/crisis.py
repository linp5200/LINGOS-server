#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 危机响应核心（§2B 危险时刻全权响应 —— 先生 2026-09-12 裁决）

设计要点（对齐框架 §2B）：
  ① 危机判定 = 确定性（代码判定——AI 不得自称危机）
  ② 系统预判类别 → 类别动作表（火灾开门 / 污染关门 / 地震开门 / 燃气开窗……）
  ③ AI 全权响应：危机期 AI 拥有基本所有设备权限（审计替代拦截）
  ④ 10 秒决策启动窗口：危机通知 AI 后 10s 内决策+启动 → 一旦动手时限解除
  ⑤ 人身安全让路铁律（第〇条）：一切行为为人身安全让路
  ⑥ 危机解除 → 回归常规 + 变更记录入审计

本模块提供：
  · detect_crisis(alert)   —— 告警 → 危机类别判定（确定性）
  · trigger_crisis(...)    —— 触发危机：状态置位 + 类别动作 + 通知（App/AI）
  · resolve_crisis(...)    —— 解除危机：状态复位 + 回归常规
  · crisis_active()        —— 供权限网关/工具执行查询（全权判定）
  · crisis_mark_moving()   —— AI 动手标记（10s 窗口解除）
  · build_crisis_prompt()  —— 危机模式系统提示段（注入 AI）
"""

import json
import logging
import os
import time
import uuid

logger = logging.getLogger("Crisis")

STATE_PATH = "/LINGOS/system/config/crisis_state.json"
ACTIONS_PATH = "/LINGOS/system/config/crisis_actions.json"
AUDIT_PATH = "/LINGOS/log/crisis_audit.jsonl"
WINDOW_SECONDS = 10   # 先生定稿：10 秒决策启动窗口（动手即解除）

# =============================================================
# 危机类别 → 中文名 / 类别动作表（§2B.3 ②——代码执行）
# =============================================================
CRISIS_NAMES = {
    "fire":       {"zh": "火灾", "en": "Fire"},
    "pollution":  {"zh": "污染", "en": "Pollution"},
    "earthquake": {"zh": "地震", "en": "Earthquake"},
    "intrusion":  {"zh": "入侵", "en": "Intrusion"},
    "gas":        {"zh": "燃气泄漏", "en": "Gas Leak"},
    "water":      {"zh": "水浸", "en": "Water Leak"},
    "sos":        {"zh": "人身求助", "en": "SOS"},
}

# 默认类别动作表（可在 crisis_actions.json 中覆盖/扩展）
# match = HA 实体匹配（entity_id/name 子串，'|' 分隔多关键词）
# special = 内置动作类型（tts_evac 等——由本模块执行）
DEFAULT_ACTIONS = {
    "fire": [
        {"id": "open_exit_doors", "desc": "打开逃生大门", "domain": "lock", "service": "unlock", "match": "door|门|gate", "always": True},
        {"id": "close_gas_valve", "desc": "关闭燃气阀", "domain": "switch", "service": "turn_off", "match": "gas|燃气"},
        {"id": "emergency_lights", "desc": "应急照明全开", "domain": "light", "service": "turn_on", "match": ".*"},
        {"id": "evac_broadcast", "desc": "撤离语音广播", "special": "tts_evac", "text": "火灾警报！请立即从逃生门撤离！"},
        {"id": "sprinkler", "desc": "启动灭火程序", "domain": "switch", "service": "turn_on", "match": "sprinkler|灭火|fire_"},
    ],
    "pollution": [
        {"id": "close_all_doors", "desc": "关闭所有大门", "domain": "lock", "service": "lock", "match": "door|门|gate", "always": True},
        {"id": "close_windows", "desc": "关闭所有窗户", "domain": "cover", "service": "close_cover", "match": "window|窗"},
        {"id": "stop_ventilation", "desc": "停止新风/空调", "domain": "climate", "service": "turn_off", "match": ".*"},
        {"id": "seal_broadcast", "desc": "封堵指引语音", "special": "tts_evac", "text": "污染警报！请关闭并封堵门窗，留在室内！"},
    ],
    "earthquake": [
        {"id": "open_exit_doors", "desc": "打开逃生门（防变形卡死）", "domain": "lock", "service": "unlock", "match": "door|门|gate", "always": True},
        {"id": "close_gas_valve", "desc": "关闭燃气阀", "domain": "switch", "service": "turn_off", "match": "gas|燃气"},
        {"id": "stop_elevator", "desc": "电梯停靠（禁乘）", "domain": "switch", "service": "turn_off", "match": "elevator|电梯"},
        {"id": "quake_broadcast", "desc": "躲避提示广播", "special": "tts_evac", "text": "地震！请就地躲避，远离窗户与高柜！"},
    ],
    "intrusion": [
        {"id": "lock_entrances", "desc": "锁闭所有入口", "domain": "lock", "service": "lock", "match": "door|门|gate", "always": True},
        {"id": "deterrence_lights", "desc": "开灯震慑", "domain": "light", "service": "turn_on", "match": ".*"},
        {"id": "deterrence_sound", "desc": "威慑语音", "special": "tts_evac", "text": "警告！非法入侵已记录，安保已通知！"},
    ],
    "gas": [
        {"id": "open_windows", "desc": "开窗通风", "domain": "cover", "service": "open_cover", "match": "window|窗"},
        {"id": "close_gas_valve", "desc": "关闭燃气阀", "domain": "switch", "service": "turn_off", "match": "gas|燃气", "always": True},
        {"id": "no_electricity", "desc": "禁用电器（防爆）", "special": "note", "text": "不要开关任何电器——防爆！"},
        {"id": "gas_broadcast", "desc": "撤离指引语音", "special": "tts_evac", "text": "燃气泄漏！请开窗通风并立即撤离，不要开关电器！"},
    ],
    "water": [
        {"id": "close_water_valve", "desc": "关闭水阀", "domain": "switch", "service": "turn_off", "match": "water|水|valve|阀", "always": True},
        {"id": "cut_power_safe", "desc": "安全区断电", "domain": "switch", "service": "turn_off", "match": "power|电"},
    ],
    "sos": [
        {"id": "unlock_for_rescue", "desc": "门禁为救援开放（让路）", "domain": "lock", "service": "unlock", "match": "door|门|gate", "always": True},
        {"id": "sos_lights", "desc": "定位灯光引导", "domain": "light", "service": "turn_on", "match": ".*"},
        {"id": "sos_broadcast", "desc": "求助语音", "special": "tts_evac", "text": "已发出求助信号，救援通道已开放！"},
    ],
}

# =============================================================
# 状态管理
# =============================================================
def _load_state() -> dict:
    try:
        if os.path.exists(STATE_PATH):
            with open(STATE_PATH, "r", encoding="utf-8") as f:
                return json.load(f) or {}
    except Exception as e:
        logger.debug("crisis state read failed: %s", e)
    return {}


def _save_state(st: dict) -> None:
    try:
        os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump(st, f, ensure_ascii=False, indent=2)
    except Exception as e:
        logger.warning("crisis state save failed: %s", e)


def _load_actions() -> dict:
    if os.path.exists(ACTIONS_PATH):
        try:
            with open(ACTIONS_PATH, "r", encoding="utf-8") as f:
                user = json.load(f) or {}
            if isinstance(user, dict) and user:
                return user
        except Exception as e:
            logger.warning("crisis actions read failed (use defaults): %s", e)
    # 首次：写入默认（用户可编辑）
    try:
        os.makedirs(os.path.dirname(ACTIONS_PATH), exist_ok=True)
        with open(ACTIONS_PATH, "w", encoding="utf-8") as f:
            json.dump(DEFAULT_ACTIONS, f, ensure_ascii=False, indent=2)
    except Exception:
        pass
    return DEFAULT_ACTIONS


def _audit(rec: dict) -> None:
    try:
        os.makedirs(os.path.dirname(AUDIT_PATH), exist_ok=True)
        rec["ts"] = int(time.time())
        with open(AUDIT_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception:
        pass


# =============================================================
# 危机判定（确定性——代码判定，AI 不得自称危机）
# =============================================================
# alert type 数字 → 可能的危机类别（结合 level 与 source 判定）
_TYPE_MAP = {
    2: "earthquake",   # ALERT_TYPE_EARTHQUAKE
    6: "fire",         # ALERT_TYPE_FIRE
    8: "intrusion",    # ALERT_TYPE_SECURITY
}

# source/description 关键词 → 危机类别（自定义告警源）
_KEYWORD_MAP = [
    (("gas", "燃气", "lpg", "甲烷", "methane"), "gas"),
    (("water", "水浸", "leak", "漏水", "flood", "洪水"), "water"),
    (("smoke", "烟", "fire", "火"), "fire"),
    (("intruder", "入侵", "break", "撬", "motion"), "intrusion"),
    (("sos", "求助", "跌倒", "fall", "emergency"), "sos"),
    (("pollution", "污染", "chem", "辐射", "radiation", "生化"), "pollution"),
]


def detect_crisis(alert: dict) -> str:
    """告警 → 危机类别（''=非危机）。

    确定性规则（防 AI 自称危机 / 防误判常态化）：
      · type 直映射（地震/火灾/安全）+ 等级门槛
      · source/description 关键词补充（燃气/水浸/污染/SOS——自定义告警源）
      · 等级门槛：fire/earthquake/intrusion level>=3；其余关键词命中即危机
    """
    try:
        atype = int(alert.get("type", 0) or 0)
        level = int(alert.get("level", 0) or 0)
        text = ("%s %s" % (alert.get("source", ""), alert.get("description", ""))).lower()

        # ① 类型直映射（等级门槛）
        ctype = _TYPE_MAP.get(atype, "")
        if ctype and level >= 3:
            return ctype

        # ② 关键词判定（含自定义来源；高等级门槛更低）
        for words, c in _KEYWORD_MAP:
            if any(w in text for w in words):
                if c in ("fire", "earthquake", "intrusion") and level < 3:
                    continue   # 关键词命中的高危类仍需等级
                return c
    except Exception as e:
        logger.debug("detect_crisis failed: %s", e)
    return ""


# =============================================================
# 类别动作执行（代码执行 · 秒级 · 不等 AI）
# =============================================================
def _execute_action(action: dict, executed_note: str) -> dict:
    """执行单条类别动作 → 结果记录（done / pending_no_ha / noted / failed）"""
    rec = {"id": action.get("id", ""), "desc": action.get("desc", ""), "status": "", "detail": ""}

    # special 内置动作
    special = action.get("special", "")
    if special == "tts_evac":
        rec["status"] = "noted"
        rec["detail"] = action.get("text", "")
        rec["broadcast"] = action.get("text", "")
        return rec
    if special == "note":
        rec["status"] = "noted"
        rec["detail"] = action.get("text", "")
        return rec

    # HA 设备动作
    domain = action.get("domain", "")
    service = action.get("service", "")
    if not domain or not service:
        rec["status"] = "failed"
        rec["detail"] = "action 配置不完整（缺 domain/service）"
        return rec

    try:
        from ha_integration import ha_load_config, _ha_do_control, cmd_ha_states
        cfg = ha_load_config()
        if not cfg or not cfg.get("host"):
            rec["status"] = "pending_no_ha"
            rec["detail"] = "HA 未配置——请人工执行或配置 Home Assistant"
            return rec

        match = action.get("match", ".*")
        always = bool(action.get("always", False))
        targets = []
        try:
            states_res = cmd_ha_states()
            states = states_res.get("data", []) if isinstance(states_res, dict) else []
            import re as _re
            pat = _re.compile(match)
            for s in states:
                eid = str(s.get("entity_id", ""))
                name = str(s.get("attributes", {}).get("friendly_name", "")) if isinstance(s.get("attributes"), dict) else ""
                if not eid.startswith(domain + "."):
                    continue
                if always or pat.search(eid) or (name and pat.search(name)):
                    targets.append(eid)
        except Exception as e:
            logger.debug("ha_states for crisis failed: %s", e)

        if not targets:
            # 无匹配实体——标记待处理（提示用户配置）
            rec["status"] = "pending_no_match"
            rec["detail"] = "未匹配到设备（匹配模式: %s）" % match
            return rec

        ok_n, fail_n = 0, 0
        for eid in targets[:20]:   # 上限防护
            try:
                r = _ha_do_control(cfg, domain, service, eid, "")
                if isinstance(r, dict) and r.get("status") == "ok":
                    ok_n += 1
                else:
                    fail_n += 1
            except Exception:
                fail_n += 1
        rec["status"] = "done" if fail_n == 0 else ("partial" if ok_n else "failed")
        rec["detail"] = "执行 %d 台设备%s" % (ok_n, ("（%d 失败）" % fail_n) if fail_n else "")
        rec["targets"] = targets[:20]
        return rec
    except Exception as e:
        rec["status"] = "failed"
        rec["detail"] = "执行异常: %s" % e
        return rec


# =============================================================
# 触发 / 解除
# =============================================================
def trigger_crisis(crisis_type: str, source: str = "", detail: str = "",
                   broadcast: bool = True) -> dict:
    """触发危机（代码判定后调用）。

    流程（三线并行——§2B.1）：
      ① 状态置位（危机模式开启——全权生效）
      ② 类别动作执行（代码秒级，不等 AI）
      ③ 通知广播（App 全屏告警卡事件 crisis_alert）
      ④ 记录 AI 窗口起点（10s 决策启动——动手即解除）
    """
    if crisis_type not in CRISIS_NAMES:
        return {"status": "error", "msg": "未知危机类别: %s" % crisis_type}

    now = int(time.time())
    cid = "cx_%s_%d" % (crisis_type, now)
    actions_map = _load_actions()
    actions = actions_map.get(crisis_type, DEFAULT_ACTIONS.get(crisis_type, []))

    # ① 执行类别动作
    results = []
    for a in actions:
        results.append(_execute_action(a, cid))

    # ② 状态置位
    st = {
        "active": True,
        "crisis_id": cid,
        "crisis_type": crisis_type,
        "name": CRISIS_NAMES[crisis_type].get("zh", crisis_type),
        "source": source,
        "detail": detail,
        "started_at": now,
        "window_start": now,          # 10s 决策启动窗口起点
        "window_seconds": WINDOW_SECONDS,
        "ai_moving": False,           # AI 是否已动手（动手 → 窗口解除）
        "actions": results,
        "resolved_at": 0,
    }
    _save_state(st)

    # ③ 审计
    _audit({"event": "trigger", "crisis_id": cid, "type": crisis_type,
            "source": source, "detail": detail,
            "actions": [{"desc": r["desc"], "status": r["status"]} for r in results]})

    # ④ 广播（App 全屏告警卡 + 通知中心）
    if broadcast:
        _broadcast_crisis(st, "crisis_alert")
        _notify_center(st)

    # ⑤ 【0.7.0 P2.5 B5】生命线投递（多通道并行 + ACK 强制 + 30s 重推 + 指标）
    try:
        from crisis_delivery import start_delivery
        start_delivery(st)
    except Exception as _de:
        logger.warning("crisis delivery start failed: %s", _de)

    logger.warning("CRISIS TRIGGERED: %s (%s) actions=%d", crisis_type, cid, len(results))
    return {"status": "ok", "data": st}


def resolve_crisis(reason: str = "manual", broadcast: bool = True) -> dict:
    """解除危机 → 一切设置自动回归常规（§2B 铁律⑥）。"""
    st = _load_state()
    if not st.get("active"):
        return {"status": "ok", "data": st, "msg": "当前无激活危机"}
    st["active"] = False
    st["resolved_at"] = int(time.time())
    st["resolve_reason"] = reason
    dur = st["resolved_at"] - int(st.get("started_at", st["resolved_at"]))
    _save_state(st)
    _audit({"event": "resolve", "crisis_id": st.get("crisis_id"), "reason": reason, "duration_s": dur})
    # 【0.7.0 P2.5 B5】停止投递循环（重推结束）
    try:
        from crisis_delivery import stop_delivery
        stop_delivery()
    except Exception:
        pass
    if broadcast:
        _broadcast_crisis(st, "crisis_resolved")
        try:
            from ux_ext import cmd_notify_push
            cmd_notify_push(title="危机已解除", body="系统已回归常规模式（%s）" % reason,
                            level="info", source="crisis")
        except Exception:
            pass
    logger.info("Crisis resolved: %s (%.0fs)", st.get("crisis_id", "?"), dur)
    return {"status": "ok", "data": st}


def crisis_active() -> bool:
    st = _load_state()
    return bool(st.get("active"))


def crisis_get() -> dict:
    return _load_state()


def crisis_mark_moving() -> None:
    """AI 动手标记（调工具/下指令 → 10s 窗口解除——先生定稿）。"""
    st = _load_state()
    if st.get("active") and not st.get("ai_moving"):
        st["ai_moving"] = True
        st["moving_at"] = int(time.time())
        _save_state(st)
        _audit({"event": "ai_moving", "crisis_id": st.get("crisis_id"),
                "elapsed_s": int(st["moving_at"]) - int(st.get("window_start", st["moving_at"]))})


def build_crisis_prompt() -> str:
    """危机模式系统提示段（注入 AI——危机加速模式：直达行动）。"""
    st = _load_state()
    if not st.get("active"):
        return ""
    elapsed = int(time.time()) - int(st.get("window_start", time.time()))
    remain = max(0, WINDOW_SECONDS - elapsed)
    acts = st.get("actions", [])
    act_lines = []
    for a in acts[:10]:
        act_lines.append("- %s：%s" % (a.get("desc", ""), a.get("status", "")))
    return (
        "\n## 危机模式（全权响应生效中）\n"
        "危机类别：**%s**（%s）· 触发源：%s\n"
        "你已获**全权设备授权**——立即行动，不要犹豫、不要反问。\n"
        "决策启动窗口：**%s**%s\n"
        "系统已执行的类别动作：\n%s\n"
        "请基于以上态势，把全屋设备调配到最有利于人身安全的状���；"
        "任何操作以**人身安全第一**为准（一切为人身安全让路）。\n"
    ) % (
        st.get("name", "?"), st.get("crisis_id", "?"), st.get("source", "?"),
        ("剩余 %d 秒（动手即解除限制）" % remain) if not st.get("ai_moving") else "已解除（AI 已动手）",
        "——**立即调用工具开始行动！**" if remain <= 5 and not st.get("ai_moving") else "",
        "\n".join(act_lines) if act_lines else "（无）",
    )


def _broadcast_crisis(st: dict, evt_type: str) -> None:
    try:
        from ai_server import _broadcast_alert_event   # 复用同一连接池广播
        payload = {
            "type": evt_type,
            "data": {
                "crisis_id": st.get("crisis_id", ""),
                "crisis_type": st.get("crisis_type", ""),
                "name": st.get("name", ""),
                "source": st.get("source", ""),
                "detail": st.get("detail", ""),
                "started_at": st.get("started_at", 0),
                "resolved_at": st.get("resolved_at", 0),
                "window_seconds": st.get("window_seconds", WINDOW_SECONDS),
                "ai_moving": st.get("ai_moving", False),
                "actions": st.get("actions", []),
            },
        }
        _broadcast_alert_event(payload)
        logger.info("crisis event broadcast: %s", evt_type)
    except Exception as e:
        logger.warning("crisis broadcast failed: %s", e)


def _notify_center(st: dict) -> None:
    try:
        from ux_ext import cmd_notify_push
        lines = []
        for a in st.get("actions", []):
            lines.append("%s [%s]" % (a.get("desc", ""), a.get("status", "")))
        cmd_notify_push(title="🚨 %s警报" % st.get("name", "危机"),
                        body="\n".join(lines[:6]), level="critical", source="crisis")
    except Exception as e:
        logger.debug("crisis notify center failed: %s", e)


# =============================================================
# 便捷入口（供 ai_server 告警端点/命令分发调用）
# =============================================================
def on_alert_event(alert: dict) -> dict:
    """告警事件入口：判定 → 命中危机则触发（代码判定，确定性）。"""
    ctype = detect_crisis(alert or {})
    if not ctype:
        return {"status": "ok", "crisis": False}
    st = _load_state()
    if st.get("active") and st.get("crisis_type") == ctype:
        # 同类危机进行中——只更新 detail（防重复触发）
        st["detail"] = str(alert.get("description", st.get("detail", "")))[:200]
        st["last_repeat_at"] = int(time.time())
        _save_state(st)
        return {"status": "ok", "crisis": True, "repeat": True, "data": st}
    return trigger_crisis(ctype,
                          source=str(alert.get("source", "")),
                          detail=str(alert.get("description", ""))[:200])
