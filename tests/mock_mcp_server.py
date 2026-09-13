#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MCP mock 服务器（测试用——最小 JSON-RPC over HTTP 实现）"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18911


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        ln = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(ln).decode())
        method = body.get("method", "")
        rid = body.get("id", 1)

        result = None
        if method == "initialize":
            result = {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "mock-mcp", "version": "1.0.0"},
            }
        elif method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        elif method == "tools/list":
            result = {"tools": [
                {"name": "get_time", "description": "获取当前时间",
                 "inputSchema": {"type": "object", "properties": {}, "required": []}},
                {"name": "echo", "description": "回显输入",
                 "inputSchema": {"type": "object", "properties": {"msg": {"type": "string"}}, "required": ["msg"]}},
            ]}
        elif method == "tools/call":
            params = body.get("params", {})
            tname = params.get("name", "")
            args = params.get("arguments", {})
            import time
            if tname == "get_time":
                result = {"content": [{"type": "text", "text": "mock-time: %d" % int(time.time())}]}
            elif tname == "echo":
                result = {"content": [{"type": "text", "text": "echo: %s" % args.get("msg", "")}]}
            else:
                result = {"content": [{"type": "text", "text": "unknown tool"}]}
        else:
            resp = {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "method not found"}}
            data = json.dumps(resp).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        resp = {"jsonrpc": "2.0", "id": rid, "result": result}
        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Mcp-Session-Id", "mock-session-1")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    print("mock MCP server on :%d" % PORT, flush=True)
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
