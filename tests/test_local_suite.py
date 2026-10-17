#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 本地可跑测试套件（无需真机/无需模型 API）

覆盖：
  A. 命令面路由（ext_dispatch：ingest_* / remote_status，模拟 /api/cmd 路径）
  B. ingest 管线（文本 / 本地 HTTP 网页 / RSS / 插件发现 / 错误诚实性）
  C. MCP 客户端（本地 mock MCP 服务器：initialize / tools/list / tools/call）
  D. 服务端函数（weather_code_map / alert_query.available / session_history）
  E. WebUI 静态自检（JS 语法由调用方 node --check；此处查 id 接线与导航↔页面）
  F. remote_access 状态结构

运行：python3 tests/test_local_suite.py
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = os.path.join(ROOT, "src", "python")
sys.path.insert(0, PY)

TMP = tempfile.mkdtemp(prefix="lingos_test_")
os.environ["LINGOS_INGEST_ROOT"] = os.path.join(TMP, "ingest")
PLUG = os.path.join(PY, "ingest_plugins")  # 直接指向仓库内示例插件目录（避免 tmp 复制兼容问题）
os.environ["LINGOS_INGEST_PLUGINS"] = PLUG

PASS, FAIL = [], []


def check(name, cond, detail=""):
    if cond:
        PASS.append(name)
        print("PASS", name)
    else:
        FAIL.append(name)
        print("FAIL", name, detail)


# ---------------- A. 命令面路由 ----------------
def test_dispatch():
    import ext_dispatch
    replies = []

    def reply(conn, name, resp):
        replies.append((name, resp))

    for cmd, req in [
        ("ingest_plugins", {}),
        ("ingest_list", {}),
        ("ingest_search", {"query": "测试"}),
        ("remote_status", {}),
    ]:
        ok = ext_dispatch.dispatch_ext(cmd, dict(req), reply, None)
        check("dispatch." + cmd, ok, "not handled")
        check("dispatch.reply." + cmd, replies and replies[-1][0] == cmd)
    m = ext_dispatch.EXT_MAP
    for k in ("ingest_add", "ingest_status", "ingest_list", "ingest_search",
              "ingest_plugins", "ingest_delete", "remote_status", "vnc_start", "vnc_stop"):
        check("EXT_MAP." + k, k in m)


# ---------------- B. ingest 管线 ----------------
def _serve_dir(files):
    docroot = tempfile.mkdtemp(dir=TMP)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            name = self.path.lstrip("/").split("?")[0]
            path = os.path.join(docroot, name)
            if os.path.isfile(path):
                data = open(path, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_response(404)
                self.end_headers()

    for name, content in files.items():
        open(os.path.join(docroot, name), "w", encoding="utf-8").write(content)
    srv = HTTPServer(("127.0.0.1", 0), H)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, "http://127.0.0.1:%d" % srv.server_port


def test_ingest():
    import ingest_manager as im
    # 文本
    r = im.add("", type_hint="text", title="本地测试文档",
               text="LINGOS 本地测试。智能家居中枢与 AI 助理。" * 4, wait=True)
    check("ingest.text.done", r.get("status") == "ok" and r.get("job", {}).get("status") == "done", str(r)[:200])
    check("ingest.text.search", im.search("智能家居")["count"] >= 1)
    # 插件发现（内置 6 + 自定义 1）
    ids = [p["id"] for p in im.plugin_list()]
    check("ingest.plugins.discovery", len(ids) >= 7 and "example.markdown" in ids, str(ids))
    # 本地网页
    srv, base = _serve_dir({
        "page.html": "<html><head><title>测试网页标题</title></head><body><p>这是网页正文，包含 LINGOS 关键词。</p></body></html>",
        "feed.xml": "<?xml version='1.0'?><rss><channel><title>测试订阅</title>"
                    "<item><title>条目一</title><link>http://x/1</link><description>订阅描述第一条</description></item>"
                    "</channel></rss>",
    })
    try:
        r = im.add(base + "/page.html", wait=True)
        check("ingest.webpage.done", r.get("status") == "ok" and r.get("job", {}).get("status") == "done",
              str(r.get("job", {}).get("error")))
        check("ingest.webpage.search", im.search("LINGOS")["count"] >= 1)
        r = im.add(base + "/feed.xml", type_hint="rss", wait=True)
        check("ingest.rss.done", r.get("status") == "ok" and r.get("job", {}).get("status") == "done",
              str(r.get("job", {}).get("error")))
    finally:
        srv.shutdown()
    # 错误诚实性：文件不存在 → error（不伪造成功）
    r = im.add("/nonexistent/file.pdf", type_hint="pdf", wait=True)
    check("ingest.error.honest", r.get("job", {}).get("status") == "error")


# ---------------- C. MCP 客户端（本地 mock） ----------------
def test_mcp():
    port = 18931
    proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "mock_mcp_server.py"), str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import socket as _sock
    for _ in range(50):
        try:
            _sock.create_connection(("127.0.0.1", port), timeout=0.3).close()
            break
        except Exception:
            time.sleep(0.2)
    try:
        import mcp_client as mc
        url = "http://127.0.0.1:%d" % port
        init = mc.mcp_initialize(url, headers={})
        check("mcp.initialize", bool(init) and init.get("serverInfo", {}).get("name") == "mock-mcp", str(init)[:160])
        tools = mc.mcp_tools_list(url, headers={})
        names = [t.get("name") for t in tools]
        check("mcp.tools_list", "echo" in names and "get_time" in names, str(names))
        out = mc.mcp_tool_call(url, "echo", {"msg": "hi"}, headers={})
        txt = json.dumps(out, ensure_ascii=False)
        check("mcp.tool_call", "echo: hi" in txt, txt[:200])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


# ---------------- D. 服务端函数 ----------------
def test_server_functions():
    import ai_server
    r = ai_server.cmd_weather_code_map()
    check("server.weather_code_map",
          r.get("status") == "ok" and isinstance(r.get("data"), list) and len(r["data"]) > 10)
    r = ai_server.cmd_alert_query()
    check("server.alert_query", r.get("status") == "ok" and "available" in r)
    r = ai_server.cmd_session_history("not-exist", 5)
    check("server.session_history", isinstance(r, dict) and "status" in r)
    r = ai_server.cmd_weather_current()
    check("server.weather_current.honest",
          r.get("status") in ("ok", "error") and (r.get("status") == "ok" or "msg" in r))


# ---------------- E. WebUI 静态自检 ----------------
def test_webui():
    src = open(os.path.join(ROOT, "webui", "index.html"), encoding="utf-8").read()
    scripts = re.findall(r"<script(?:[^>]*)>(.*?)</script>", src, re.S)
    js = "\n".join(scripts)
    ids = set(re.findall(r'id="([A-Za-z0-9_\-]+)"', src))
    refs = set(re.findall(r"getElementById\('([A-Za-z0-9_\-]+)'\)", js))
    pages = set(re.findall(r'id="p-([a-z\-]+)"', src))
    navs = set(re.findall(r'data-p="([a-z\-]+)"', src))
    check("webui.no_missing_ids", not (refs - ids), str(sorted(refs - ids))[:200])
    check("webui.nav_aligned", navs == pages, str(sorted(navs ^ pages))[:200])
    check("webui.has_palette", "lingosPalette" in src)
    check("webui.has_ingest_page", 'id="p-ingest"' in src and "wfIngestAdd" in src)
    check("webui.theme_layer", 'themes.css' in src and 'theme.js' in src)


# ---------------- F. remote_access ----------------
def test_remote():
    import remote_access
    r = remote_access.cmd_remote_status()
    d = r.get("data", {})
    check("remote.status", r.get("status") == "ok" and all(k in d for k in ("http", "ws", "vnc")))


if __name__ == "__main__":
    print("TMP:", TMP)
    test_dispatch()
    test_ingest()
    test_mcp()
    test_server_functions()
    test_webui()
    test_remote()
    print()
    print("RESULT: PASS=%d FAIL=%d" % (len(PASS), len(FAIL)))
    if FAIL:
        print("FAILED:", FAIL)
        sys.exit(1)
    print("ALL LOCAL TESTS PASSED")
