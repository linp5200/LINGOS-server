#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Agent Orchestrator（子 AI 对话协作编排器）
版本: LN-B-5.0.0.0-rc0.4
功能：Hub 模式多智能体协作
  - 主 AI 委派任务（delegate）→ 子 AI 独立会话执行（角色 prompt + 工具白名单 + 任务令牌）
  - 子 AI ↔ 子 AI 消息经主 AI 转发（Hub），默认 20 轮上限
  - 子 AI 白名单外操作（如 ha_write 事件创建）→ 上报主 AI 执行
  - 子 AI 独立记忆；读取他 AI 记忆需主 AI 授权
核心协议：C1（容错/日志）、C-C（容错编程）、AI-CTL（决策透明）
"""

import json
import time
import uuid
import logging
import threading
import os

logger = logging.getLogger("AgentOrchestrator")

DEFAULT_MAX_ROUNDS = 20          # 子 AI 间交流轮次上限
DEFAULT_TOKEN_TTL = 3600         # 任务令牌有效期（秒）

# 角色定义文件（可配置化：用户可修改 prompt 与工具白名单）
AGENT_ROLES_FILE = "/LINGOS/system/config/agent_roles.json"

# 内置默认角色（硬编码 fallback：文件缺失/损坏时使用）
ROLE_DEFS = {
    "ai_code": {
        "prompt": "You are ai_code, the code analysis sub-agent of LING OS. "
                  "Your duties: code analysis, generation, debugging, file inspection. "
                  "You may only use whitelisted skills. For anything outside your whitelist, "
                  "report to the main AI (Nook) instead of attempting directly.",
        "skills": ["file_read", "file_list", "file_write", "script_exec", "process_list", "memory_search"]
    },
    "ai_guard": {
        "prompt": "You are ai_guard, the security assessment sub-agent of LING OS. "
                  "Your duties: security review, risk assessment, threat analysis. "
                  "You are read-only. For actions, report to the main AI (Nook).",
        "skills": ["system_info", "process_list", "file_read", "net_status", "memory_search", "audit_log"]
    },
    "ai_general": {
        "prompt": "You are ai_general, the general-purpose sub-agent of LING OS. "
                  "Your duties: network checks, system info, config reading, information gathering. "
                  "For high-risk or non-whitelisted actions, report to the main AI (Nook).",
        "skills": ["net_ping", "net_status", "system_info", "system_memory", "system_disk",
                   "config_read", "memory_search", "read_log"]
    }
}

# 运行时角色定义（可从 agent_roles.json 覆盖）
_active_roles = dict(ROLE_DEFS)


def _ensure_roles_file():
    """首次生成默认角色定义文件（便于用户配置）"""
    try:
        if not os.path.exists(AGENT_ROLES_FILE):
            os.makedirs(os.path.dirname(AGENT_ROLES_FILE), exist_ok=True)
            with open(AGENT_ROLES_FILE, "w", encoding="utf-8") as f:
                json.dump({"roles": ROLE_DEFS, "version": "1.0"}, f,
                          ensure_ascii=False, indent=2)
            logger.info("Generated default agent roles file: %s", AGENT_ROLES_FILE)
    except Exception as e:
        logger.warning("Failed to generate agent roles file: %s", e)


def reload_roles():
    """重新加载角色定义（agent_roles.json；缺失/损坏回退内置默认）"""
    global _active_roles
    _ensure_roles_file()
    try:
        if os.path.exists(AGENT_ROLES_FILE):
            with open(AGENT_ROLES_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            roles = data.get("roles", {})
            if isinstance(roles, dict) and roles:
                merged = dict(ROLE_DEFS)
                for name, cfg in roles.items():
                    if not isinstance(cfg, dict):
                        continue
                    entry = dict(merged.get(name, {}))
                    if isinstance(cfg.get("prompt"), str) and cfg["prompt"].strip():
                        entry["prompt"] = cfg["prompt"].strip()
                    if isinstance(cfg.get("skills"), list):
                        entry["skills"] = [s for s in cfg["skills"] if isinstance(s, str)]
                    merged[name] = entry
                _active_roles = merged
                logger.info("Loaded %d agent roles from %s", len(merged), AGENT_ROLES_FILE)
                return
    except Exception as e:
        logger.warning("Failed to load agent roles file, using defaults: %s", e)
    _active_roles = dict(ROLE_DEFS)


class AgentSession:
    """单个子 AI 任务会话（独立上下文 + 白名单 + 令牌）"""

    def __init__(self, task_id, role, task_desc, auth_token, max_rounds=DEFAULT_MAX_ROUNDS):
        self.task_id = task_id
        self.role = role
        self.task_desc = task_desc
        self.auth_token = auth_token
        self.max_rounds = max_rounds
        self.round = 0
        self.created_at = time.time()
        self.messages = []          # 独立对话上下文
        self.status = "pending"     # pending | running | waiting_main | completed | failed
        self.result = ""
        self.dialogue_log = []      # 交流记录（审计/显示用）
        self.pending_events = []    # 【优化2】待推送过程事件（思考/工具/结果，线程安全由 GIL 保证）
        role_info = _active_roles.get(role, _active_roles.get("ai_general", ROLE_DEFS["ai_general"]))
        self.system_prompt = role_info["prompt"] + "\nYour task: " + task_desc
        self.skill_whitelist = set(role_info["skills"])

    def allowed(self, skill_name):
        """白名单检查（安全冗余：默认拒绝）"""
        return skill_name in self.skill_whitelist

    def add_message(self, role, content):
        self.messages.append({"role": role, "content": content})

    def log_dialogue(self, from_agent, to_agent, msg_type, content):
        self.dialogue_log.append({
            "time": time.strftime("%H:%M:%S"),
            "from": from_agent, "to": to_agent, "type": msg_type,
            "content": (content or "")[:200]
        })


class AgentOrchestrator:
    """Hub 模式编排器（单例）"""

    def __init__(self):
        self.sessions = {}          # task_id -> AgentSession
        self.lock = threading.Lock()
        self.max_rounds = DEFAULT_MAX_ROUNDS

    # ========== 会话管理 ==========

    def create_session(self, role, task_desc):
        """主 AI 委派：创建子 AI 会话并签发任务令牌"""
        task_id = "sub_ai_%s_%s" % (role, uuid.uuid4().hex[:8])
        auth_token = uuid.uuid4().hex[:16]
        with self.lock:
            self.sessions[task_id] = AgentSession(
                task_id, role, task_desc, auth_token, self.max_rounds)
        logger.info("Agent session created: %s (role=%s)", task_id, role)
        return task_id, auth_token

    def get_session(self, task_id):
        with self.lock:
            return self.sessions.get(task_id)

    def session_status_all(self):
        """所有子 AI 任务状态（供用户显示）"""
        with self.lock:
            return [
                {"task_id": s.task_id, "role": s.role, "status": s.status,
                 "round": s.round, "max_rounds": s.max_rounds}
                for s in self.sessions.values()
            ]

    # ========== 消息路由（Hub：子 AI 间消息经主 AI） ==========

    def route(self, msg):
        """处理子 AI 消息：delegate / reply / ask / report

        :param msg: {"from":..., "to":..., "type":..., "task_id":..., "content":...}
        :return: (处理结果, 是否需要主 AI 介入)
        """
        msg_type = msg.get("type", "report")
        task_id = msg.get("task_id", "")
        content = msg.get("content", "")
        session = self.get_session(task_id)

        if not session:
            return {"status": "error", "message": "unknown task_id"}, False

        if session.round >= session.max_rounds:
            session.status = "completed"
            return {"status": "error", "message": "max rounds reached"}, True

        session.round += 1
        session.log_dialogue(msg.get("from", "?"), msg.get("to", "nook"), msg_type, content)

        if msg_type == "delegate":
            session.status = "running"
            return {"status": "ok", "message": "delegated"}, False
        if msg_type == "reply":
            session.add_message("assistant", content)
            session.status = "running"
            return {"status": "ok", "message": "replied"}, False
        if msg_type == "ask":
            # 子 AI 向主 AI 请示（决策/授权/白名单外操作）→ 主 AI 介入
            session.status = "waiting_main"
            return {"status": "ok", "need_main": True, "message": content}, True
        if msg_type == "report":
            session.status = "completed"
            session.result = content
            return {"status": "ok", "message": "reported", "result": content}, False
        return {"status": "error", "message": "unknown type"}, False

    # ========== 白名单外操作上报（子 AI → 主 AI 执行） ==========

    def on_skill_denied(self, task_id, skill_name, args):
        """子 AI 请求白名单外技能（如 ha_write）→ 生成上报请求交主 AI 执行"""
        session = self.get_session(task_id)
        if not session:
            return None
        session.status = "waiting_main"
        session.log_dialogue(session.role, "nook", "skill_denied",
                             "skill=%s args=%s" % (skill_name, json.dumps(args)[:200]))
        return {
            "need_main": True,
            "reason": "skill_not_allowed",
            "skill": skill_name,
            "args": args,
            "task_id": task_id,
            "agent": session.role
        }

    # ========== 子 AI 执行器（独立推理 + 白名单约束 + 20 轮上限） ==========

    def run_sub_agent(self, task_id, prompt):
        """在线程中执行子 AI 任务（简化 ReAct，最多 3 轮工具迭代；白名单外上报主 AI）

        :param task_id: orchestrator 会话 ID
        :param prompt: 任务描述
        """
        session = self.get_session(task_id)
        if not session:
            return

        def _run():
            session.status = "running"
            try:
                import sub_ai_scheduler as sched
                from skill_handlers import execute_skill

                msgs = [{"role": "system", "content": session.system_prompt},
                        {"role": "user", "content": prompt}]

                final = ""
                for _ in range(3):
                    resp = sched.call_deepseek_nonstream(msgs, timeout=60)
                    content = resp.get("content") or (resp.get("message") or {}).get("content", "")
                    tool_calls = resp.get("tool_calls") or (resp.get("message") or {}).get("tool_calls", [])

                    # 【优化2】思考事件推送
                    if content:
                        session.pending_events.append({"type": "thinking", "content": content[:300]})

                    if not tool_calls:
                        final = content
                        break

                    results = []
                    for tc in tool_calls:
                        fn = tc.get("function", {})
                        name = fn.get("name", "")
                        try:
                            args = json.loads(fn.get("arguments", "{}"))
                        except Exception:
                            args = {}

                        # 【优化2】工具调用事件
                        session.pending_events.append({"type": "tool_call",
                                                       "content": f"{name} {json.dumps(args, ensure_ascii=False)[:150]}"})

                        if not session.allowed(name):
                            # 【安全冗余】白名单外操作（如 ha_write）→ 上报主 AI，不直接执行
                            self.on_skill_denied(task_id, name, args)
                            session.pending_events.append({"type": "tool_result",
                                                           "content": f"[NEED_MAIN] {name} 需主AI处理", "success": 0})
                            final = "[NEED_MAIN:%s] requires main AI (delegate denied by whitelist)" % name
                            session.status = "waiting_main"
                            session.result = final
                            return

                        ok, out = execute_skill(name, json.dumps(args))
                        # 【优化2】工具结果事件
                        session.pending_events.append({"type": "tool_result",
                                                       "content": (out if ok else "Error: " + out)[:300],
                                                       "success": 1 if ok else 0})
                        results.append({
                            "tool_call_id": tc.get("id", ""),
                            "role": "tool",
                            "content": out if ok else ("Error: " + out)
                        })

                    msgs.append({"role": "assistant", "content": content or ""})
                    msgs.extend(results)

                session.result = final
                session.status = "completed" if final else "failed"
                session.add_message("assistant", final)
                # 【优化2】完成事件
                session.pending_events.append({"type": "completed", "content": final[:500]})
                logger.info("Sub agent %s finished: %s", task_id, session.status)
            except Exception as e:
                logger.error("Sub agent run error: %s", e)
                session.status = "failed"
                session.result = str(e)

        threading.Thread(target=_run, daemon=True).start()


# ========== 全局单例 ==========
_orchestrator = None


def get_orchestrator() -> AgentOrchestrator:
    global _orchestrator
    if _orchestrator is None:
        reload_roles()   # 【可配置化】首次加载 agent_roles.json（或生成默认）
        _orchestrator = AgentOrchestrator()
    return _orchestrator
