#!/usr/bin/env python3
"""lingos-web 0.13 冒烟/负向测试（unittest）。"""
import http.client
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "services" / "lingos_web"))
import server  # noqa: E402


class WebTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="lingos-web-test-")
        cls.store = server.Store(Path(cls.tmp))
        server.Handler.store = cls.store
        server.Handler.manager = server.DeviceManager(cls.store)
        server.Handler.web_dir = HERE / "webdesk"
        server.Handler.lan = False
        server.Handler.pair_code = ""
        cls.httpd = server.ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        cls.port = cls.httpd.server_address[1]
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.thread.start()
        cls.cookie = ""

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def req(self, method, path, body=None, extra_headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = dict(extra_headers or {})
        if body is not None:
            headers["Content-Type"] = "application/json"
        cookie = getattr(WebTest, "cookie", "")
        if cookie:
            headers["Cookie"] = cookie
        conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
        r = conn.getresponse()
        data = r.read().decode("utf-8", "replace")
        set_cookie = r.getheader("Set-Cookie") or ""
        if "lingos_session=" in set_cookie:
            WebTest.cookie = set_cookie.split(";", 1)[0]
        conn.close()
        return r.status, data

    def test_01_health(self):
        st, body = self.req("GET", "/api/health")
        self.assertEqual(st, 200)
        self.assertTrue(json.loads(body)["ok"])

    def test_02_setup_and_unauth(self):
        st, body = self.req("POST", "/api/setup", {"username": "admin", "password": "testpass123"})
        self.assertEqual(st, 200)
        WebTest.root_key = json.loads(body).get("root_key", "")
        # new connection without cookie must be 401
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.request("GET", "/api/devices")
        r = conn.getresponse()
        r.read()
        conn.close()
        self.assertEqual(r.status, 401)

    def test_03_second_setup_rejected(self):
        st, _ = self.req("POST", "/api/setup", {"username": "x", "password": "12345678"})
        self.assertEqual(st, 409)

    def test_04_wrong_login(self):
        st, _ = self.req("POST", "/api/login", {"username": "admin", "password": "wrongpass"})
        self.assertEqual(st, 401)

    def test_05_devices(self):
        st, body = self.req("GET", "/api/devices")
        self.assertEqual(st, 200)
        self.assertGreaterEqual(len(json.loads(body)["devices"]), 3)

    def test_06_traversal(self):
        st, _ = self.req("GET", "/api/fs/read?path=/../../etc/passwd")
        self.assertEqual(st, 400)

    def test_07_write_read(self):
        st, _ = self.req("POST", "/api/fs/write", {"path": "/note.txt", "content": "hello"})
        self.assertEqual(st, 200)
        st, body = self.req("GET", "/api/fs/read?path=/note.txt")
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["content"], "hello")

    def test_08_security_level(self):
        st, _ = self.req("POST", "/api/security/level", {"level": "L2"})
        self.assertEqual(st, 200)
        st, body = self.req("GET", "/api/security")
        self.assertEqual(json.loads(body)["level"], "L2")

    def test_09_add_virtual_device_and_command(self):
        st, body = self.req("POST", "/api/devices", {"name": "测试灯", "type": "light"})
        self.assertEqual(st, 200)
        did = json.loads(body)["device"]["id"]
        st, _ = self.req("POST", f"/api/devices/{did}/command", {"cmd": "toggle"})
        self.assertEqual(st, 200)
        st, body = self.req("GET", "/api/devices")
        self.assertTrue(any(d["id"] == did for d in json.loads(body)["devices"]))

    def test_10_adapters_and_settings(self):
        st, body = self.req("GET", "/api/adapters")
        self.assertEqual(st, 200)
        self.assertTrue(any(a["protocol"] == "virtual" for a in json.loads(body)["adapters"]))
        st, _ = self.req("POST", "/api/settings", {"mqtt_host": "127.0.0.1"})
        self.assertEqual(st, 200)
        st, body = self.req("GET", "/api/settings")
        self.assertEqual(json.loads(body)["settings"]["mqtt_host"], "127.0.0.1")

    def raw_get(self, path):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {"Cookie": WebTest.cookie} if WebTest.cookie else {}
        conn.request("GET", path, headers=headers)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, r.getheader("Content-Type") or "", data

    def test_13_audit_export_and_backup(self):
        st, ctype, data = self.raw_get("/api/audit/export")
        self.assertEqual(st, 200)
        self.assertIn("application/json", ctype)
        self.assertIn("events", json.loads(data.decode("utf-8")))
        st, ctype, data = self.raw_get("/api/backup")
        self.assertEqual(st, 200)
        self.assertIn("gzip", ctype)
        self.assertEqual(data[:2], b"\x1f\x8b")

    def raw_post(self, path, data, headers):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)
        h = dict(headers or {})
        h["Content-Type"] = "application/gzip"
        if WebTest.cookie:
            h["Cookie"] = WebTest.cookie
        conn.request("POST", path, body=data, headers=h)
        r = conn.getresponse()
        body = r.read().decode("utf-8", "replace")
        st = r.status
        conn.close()
        return st, body

    def test_14_restore_backup(self):
        st, _ = self.req("POST", "/api/fs/write", {"path": "/restore.txt", "content": "before"})
        self.assertEqual(st, 200)
        st, body = self.req("POST", "/api/devices", {"name": "before-restore", "type": "light"})
        self.assertEqual(st, 200)
        st, ctype, backup = self.raw_get("/api/backup")
        self.assertEqual(st, 200)
        self.assertIn("gzip", ctype)
        self.assertGreater(len(backup), 50)
        st, _ = self.req("POST", "/api/fs/write", {"path": "/restore.txt", "content": "after"})
        self.assertEqual(st, 200)
        st, _ = self.req("POST", "/api/devices", {"name": "after-restore", "type": "light"})
        self.assertEqual(st, 200)
        st, body = self.raw_post("/api/restore", backup, {"X-Lingos-Root-Key": WebTest.root_key})
        self.assertEqual(st, 200, body)
        self.assertTrue(json.loads(body)["ok"])
        st, body = self.req("GET", "/api/fs/read?path=/restore.txt")
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["content"], "before")
        st, body = self.req("GET", "/api/devices")
        names = [d["name"] for d in json.loads(body)["devices"]]
        self.assertIn("before-restore", names)
        self.assertNotIn("after-restore", names)
        st, _ = self.raw_post("/api/restore", b"not-a-gzip", {"X-Lingos-Root-Key": WebTest.root_key})
        self.assertEqual(st, 400)
        st, _ = self.raw_post("/api/restore", backup, {})
        self.assertEqual(st, 403)

    def test_12_root_key_and_api_token(self):
        st, _ = self.req("POST", "/api/acl", {"subject": "bob", "path": "/x", "perm": "read"})
        self.assertEqual(st, 403)
        self.assertTrue(WebTest.root_key)
        st, _ = self.req("POST", "/api/acl", {"subject": "bob", "path": "/x", "perm": "read"},
                         extra_headers={"X-Lingos-Root-Key": WebTest.root_key})
        self.assertEqual(st, 200)
        st, body = self.req("POST", "/api/token", {"name": "test-app"})
        self.assertEqual(st, 200)
        token = json.loads(body)["token"]
        st, body = self.req("GET", "/api/me", extra_headers={"Authorization": "Bearer " + token})
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["username"], "admin")
        # Mirrored for C HTTP 18080 / WS 2939 strict token verification.
        tf = Path(self.tmp) / "state" / "tokens.json"
        self.assertTrue(tf.is_file())
        data = json.loads(tf.read_text(encoding="utf-8"))
        self.assertTrue(any(x.get("token") == token and x.get("revoked") == 0 for x in data))

    def test_11_acl_enforced_for_user(self):
        self.store.create_user("bob", "bobpass123", "user")
        bob = self.store.verify_user("bob", "bobpass123")
        self.assertIsNotNone(bob)
        self.assertFalse(self.store.acl_check(bob, "/secret", "read"))
        self.store.acl_set("bob", "/secret", "read")
        self.assertTrue(self.store.acl_check(bob, "/secret/file", "read"))
        self.assertFalse(self.store.acl_check(bob, "/secret/file", "write"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
