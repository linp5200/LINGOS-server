"""agent_runtime 静态单测（python3 -m unittest）。"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src", "python"))
from agent_runtime import AgentRuntime, PermissionGate, ToolCall, ToolResult, ToolSpec


def handler(call):
    return ToolResult(ok=True, data={"echo": call.args.get("q", "")})


class GateTest(unittest.TestCase):
    def test_l1_network_allowed(self):
        gate = PermissionGate("L1")
        spec = ToolSpec(id="tool.net", capabilities=["network"])
        ok, need, _ = gate.check(spec, ToolCall("tool.net", "查天气", "city=北京"))
        self.assertTrue(ok)
        self.assertFalse(need)

    def test_l2_network_needs_confirm(self):
        gate = PermissionGate("L2")
        spec = ToolSpec(id="tool.net", capabilities=["network"])
        ok, need, _ = gate.check(spec, ToolCall("tool.net", "查天气", "city=北京"))
        self.assertFalse(ok)
        self.assertTrue(need)

    def test_l4_readonly_needs_confirm(self):
        gate = PermissionGate("L4")
        spec = ToolSpec(id="tool.read", capabilities=["read"])
        ok, need, _ = gate.check(spec, ToolCall("tool.read", "读文件", "path=/tmp/a"))
        self.assertFalse(ok)
        self.assertTrue(need)

    def test_missing_scope(self):
        gate = PermissionGate("L1")
        spec = ToolSpec(id="tool.x", capabilities=["compute"])
        ok, need, reason = gate.check(spec, ToolCall("tool.x", "p", ""))
        self.assertFalse(ok)
        self.assertIn("scope", reason)


class RuntimeTest(unittest.TestCase):
    def test_tool_executed(self):
        rt = AgentRuntime("L1")
        rt.register(ToolSpec(id="tool.echo", capabilities=["compute"]), handler)
        out = rt.run("测试", [ToolCall("tool.echo", "回显", "s", {"q": "hi"})])
        self.assertEqual(out["trace"][0]["tool"], "tool.echo")
        self.assertTrue(out["trace"][0]["ok"])
        self.assertEqual(out["denied"], 0)

    def test_denied_recorded(self):
        rt = AgentRuntime("L4")
        rt.register(ToolSpec(id="tool.net", capabilities=["network"]), handler)
        out = rt.run("测试", [ToolCall("tool.net", "联网", "s")])
        self.assertFalse(out["trace"][0]["allow"])
        self.assertTrue(out["trace"][0]["need_confirm"])
        self.assertEqual(out["denied"], 1)

    def test_confirmed_allows(self):
        rt = AgentRuntime("L4")
        rt.register(ToolSpec(id="tool.read", capabilities=["read"]), handler)
        out = rt.run("测试", [ToolCall("tool.read", "读取", "s")],
                     confirmed_tools={"tool.read"})
        self.assertTrue(out["trace"][0]["allow"])


if __name__ == "__main__":
    unittest.main()
