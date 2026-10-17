#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""聊天/预警契约冒烟测试（无网络、无模型调用）

覆盖本轮 P0 修复的契约点：
  1. _send_evt 自动注入 ts/seq（时间归属与排序）
  2. chat 流事件含 round_start / round_end（客户端分段边界）
  3. 预警级别归一化：L0=紧急通道，数字 3=L3，0=info
  4. session_history 拒绝空 id 兜底（源码契约存在）
运行：python3 tests/test_chat_contract.py
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = os.path.join(ROOT, "src", "python")
sys.path.insert(0, PY)

fail = []


def check(name, cond, detail=""):
    if cond:
        print("PASS", name)
    else:
        print("FAIL", name, detail)
        fail.append(name)


def test_send_evt():
    import ai_server

    class FakeConn:
        def __init__(self):
            self.lines = []

        def send(self, b):
            self.lines.append(b.decode("utf-8").strip())

    c = FakeConn()
    ai_server._send_evt(c, {"type": "content", "delta": "hello"})
    ai_server._send_evt(c, {"type": "tool_call", "name": "x", "args": "{}"})
    evs = [json.loads(x) for x in c.lines]
    check("send_evt.ts", all("ts" in e for e in evs))
    check("send_evt.seq", len(set(e.get("seq") for e in evs)) == 2)
    check("send_evt.type", evs[0]["type"] == "content" and evs[1]["type"] == "tool_call")


def test_round_events():
    src = open(os.path.join(PY, "ai_server.py"), encoding="utf-8").read()
    check("round_start emitted", '"type": "round_start"' in src)
    check("round_end emitted", '"type": "round_end"' in src)


def test_alert_levels():
    import alert_store
    n = alert_store.normalize_level
    check("L0 urgent", n("L0") == "L0", n("L0"))
    check("int 3 -> L3", n(3) == "L3", n(3))
    check("str L5", n("L5") == "L5", n("L5"))
    check("int 0 -> info", n(0) == "info", n(0))
    check("None -> info", n(None) == "info", n(None))


def test_session_history_param():
    src = open(os.path.join(PY, "ai_server.py"), encoding="utf-8").read()
    check("session_history accepts session_id",
          'req.get("session_id")' in src and "session_history" in src)


if __name__ == "__main__":
    test_send_evt()
    test_round_events()
    test_alert_levels()
    test_session_history_param()
    test_reply_trace()
    print()
    if fail:
        print("FAILED:", len(fail), fail)
        sys.exit(1)
    print("ALL CONTRACT TESTS PASSED")


def test_reply_trace():
    import ai_server

    class FakeConn:
        def __init__(self):
            self.lines = []

        def send(self, b):
            self.lines.append(b.decode('utf-8').strip())

    c = FakeConn()
    ai_server._reply(c, 'system_info', {'status': 'ok', 'data': {'version': 'x'}})
    ev = json.loads(c.lines[-1])
    check('reply.trace_id', bool(ev.get('trace_id')))
    check('reply.duration_ms', 'duration_ms' in ev)
    check('reply.server_ts', 'server_ts' in ev)
    check('reply.cmd', ev.get('cmd') == 'system_info')
