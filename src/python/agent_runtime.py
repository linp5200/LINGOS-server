"""LING OS Agent Runtime（DSH 风格骨架）。

设计要点（借鉴 DSH）：
- 计划 -> 工具调用 -> 权限闸门 -> 观察 -> 反思，可追踪；
- 工具 manifest 声明 capabilities/data_scope/risk/confirmation；
- 按隐私等级 L0-L4 决定自动/确认/拒绝；
- 高风险操作必须由调用方补充用户确认；
- 全程可审计。

本模块不依赖外部库，便于静态校验与单测。
"""
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional


LEVELS = ("L0", "L1", "L2", "L3", "L4")

# 自动允许（无需确认）的能力
AUTO_ALLOW: Dict[str, set] = {
    "L0": {"read", "compute"},
    "L1": {"read", "compute", "network"},
    "L2": {"read", "compute"},
    "L3": set(),
    "L4": set(),
}
# 必须确认的能力
NEED_CONFIRM: Dict[str, set] = {
    "L0": {"write", "exec", "device"},
    "L1": {"write", "exec", "device"},
    "L2": {"write", "network", "exec", "device"},
    "L3": {"read", "compute", "write", "network", "exec", "device"},
    "L4": {"read", "compute", "write", "network", "exec", "device"},
}


@dataclass
class ToolSpec:
    id: str
    capabilities: List[str] = field(default_factory=list)
    risk: str = "low"
    confirmation: str = "auto"
    data_scope: List[str] = field(default_factory=list)


@dataclass
class ToolCall:
    tool_id: str
    purpose: str
    scope: str
    args: Dict[str, Any] = field(default_factory=dict)


@dataclass
class ToolResult:
    ok: bool
    data: Any = None
    error: str = ""


class PermissionGate:
    """按等级做权限判定；返回 (allowed, need_confirm, reason)。"""

    def __init__(self, level: str = "L1"):
        self.level = level if level in LEVELS else "L1"

    def check(self, spec: ToolSpec, call: ToolCall,
              confirmed: bool = False) -> "tuple[bool, bool, str]":
        if not call.purpose or not call.scope:
            return False, False, "missing purpose/scope"
        caps = set(spec.capabilities)
        need = caps & NEED_CONFIRM[self.level]
        if need and not confirmed:
            return False, True, "需要用户确认: %s" % ",".join(sorted(need))
        auto = caps & AUTO_ALLOW[self.level]
        if not caps:
            return True, False, "ok"
        if caps and not auto and not need:
            return False, False, "等级 %s 不允许该工具" % self.level
        return True, False, "ok"


class AgentRuntime:
    """最小可运行 Agent 循环：plan -> call -> observe -> reflect。"""

    def __init__(self, level: str = "L1",
                 audit: Optional[Callable[[str, str], None]] = None):
        self.level = level
        self.gate = PermissionGate(level)
        self.tools: Dict[str, ToolSpec] = {}
        self.handlers: Dict[str, Callable[[ToolCall], ToolResult]] = {}
        self.audit = audit or (lambda action, detail: None)

    def register(self, spec: ToolSpec,
                 handler: Callable[[ToolCall], ToolResult]) -> None:
        self.tools[spec.id] = spec
        self.handlers[spec.id] = handler

    def run(self, goal: str, plan: List[ToolCall],
            confirmed_tools: Optional[set] = None,
            max_steps: int = 16) -> Dict[str, Any]:
        confirmed_tools = confirmed_tools or set()
        trace: List[Dict[str, Any]] = []
        self.audit("agent-run", "goal=%s level=%s" % (goal, self.level))
        for i, call in enumerate(plan[:max_steps]):
            spec = self.tools.get(call.tool_id)
            if spec is None:
                trace.append({"step": i, "tool": call.tool_id,
                              "allow": False, "reason": "unknown tool"})
                continue
            allowed, need_confirm, reason = self.gate.check(
                spec, call, confirmed=call.tool_id in confirmed_tools)
            entry = {"step": i, "tool": call.tool_id, "purpose": call.purpose,
                     "scope": call.scope, "allow": allowed, "reason": reason}
            if not allowed:
                entry["need_confirm"] = need_confirm
                trace.append(entry)
                self.audit("agent-deny", "%s reason=%s" % (call.tool_id, reason))
                continue
            try:
                result = self.handlers[call.tool_id](call)
            except Exception as exc:  # tool 失败不应拖垮 agent
                result = ToolResult(ok=False, error=str(exc))
            entry["ok"] = result.ok
            entry["data"] = result.data
            entry["error"] = result.error
            trace.append(entry)
            self.audit("agent-tool", "%s ok=%s" % (call.tool_id, result.ok))
        return {"goal": goal, "level": self.level, "trace": trace,
                "denied": sum(1 for t in trace if not t.get("allow"))}
