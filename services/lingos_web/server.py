#!/usr/bin/env python3
"""LING OS Web Desktop server (0.13).

Single-file, standard-library only. Serves the Web desktop from webdesk/ and a
small authenticated API. Designed for Termux/proot (no systemd, no root).

Security defaults:
  * binds 127.0.0.1 unless --lan is given;
  * first-run setup is local-only (or needs the printed pair code);
  * session cookie HttpOnly/SameSite=Strict;
  * file API is confined to LINGOS/data/files and enforced by an app-level ACL;
  * no write into /LINGOS/bin|lib|python|system.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import shutil
import socket
import sqlite3
import sys
import tarfile
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lingos_device.manager import DeviceManager

VERSION = "0.13.0"
COOKIE = "lingos_session"
SESSION_TTL = 7 * 24 * 3600
MAX_BODY = 8 * 1024 * 1024
LEVELS = {
    "L0": ("关闭", "不做限制，兼容优先"),
    "L1": ("常规", "推荐默认，阻止常见跟踪"),
    "L2": ("限制", "默认拒绝出站，白名单放行"),
    "L3": ("严格", "最小权限，设备/网络默认关闭"),
    "L4": ("MAX", "逐次手动批准（高级）"),
}


def now() -> float:
    return time.time()


def iso(ts: float | None = None) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts or now()))


def log(*a) -> None:
    print("[lingos-web]", *a, flush=True)


class Store:
    def __init__(self, root: Path):
        self.root = root
        self.data = root / "data"
        self.files = self.data / "files"
        self.data.mkdir(parents=True, exist_ok=True)
        self.files.mkdir(parents=True, exist_ok=True)
        self.db_path = self.data / "webdesk.db"
        self.lock = threading.Lock()
        self.conn = sqlite3.connect(str(self.db_path), check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self._init()

    def _init(self) -> None:
        with self.lock:
            c = self.conn
            c.executescript(
                """
                CREATE TABLE IF NOT EXISTS users(
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  username TEXT UNIQUE NOT NULL,
                  salt TEXT NOT NULL,
                  hash TEXT NOT NULL,
                  role TEXT NOT NULL DEFAULT 'user',
                  created REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS sessions(
                  token TEXT PRIMARY KEY,
                  user_id INTEGER NOT NULL,
                  expires REAL NOT NULL,
                  ip TEXT
                );
                CREATE TABLE IF NOT EXISTS acl(
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  subject TEXT NOT NULL,
                  path TEXT NOT NULL,
                  perm TEXT NOT NULL,
                  UNIQUE(subject, path)
                );
                CREATE TABLE IF NOT EXISTS devices(
                  id TEXT PRIMARY KEY,
                  name TEXT NOT NULL,
                  type TEXT NOT NULL,
                  protocol TEXT NOT NULL,
                  online INTEGER NOT NULL DEFAULT 1,
                  state TEXT NOT NULL DEFAULT '{}'
                );
                CREATE TABLE IF NOT EXISTS events(
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  ts REAL NOT NULL,
                  kind TEXT NOT NULL,
                  detail TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS settings(
                  key TEXT PRIMARY KEY,
                  value TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS api_tokens(
                  token_hash TEXT PRIMARY KEY,
                  user_id INTEGER NOT NULL,
                  name TEXT NOT NULL,
                  created REAL NOT NULL,
                  revoked INTEGER NOT NULL DEFAULT 0
                );
                """
            )
            c.commit()
            if c.execute("SELECT COUNT(*) FROM devices").fetchone()[0] == 0:
                seed = [
                    ("light.living", "客厅灯", "light", "virtual", 1, {"on": False, "brightness": 80}),
                    ("lock.front", "入户门锁", "lock", "virtual", 1, {"locked": True}),
                    ("sensor.temp", "温度传感器", "sensor", "virtual", 1, {"temperature": 24.5, "humidity": 52}),
                    ("sensor.smoke", "烟雾传感器", "sensor", "virtual", 0, {"smoke": False}),
                    ("camera.door", "门口摄像头", "camera", "virtual", 0, {"stream": ""}),
                ]
                c.executemany(
                    "INSERT INTO devices(id,name,type,protocol,online,state) VALUES(?,?,?,?,?,?)",
                    [(i, n, t, p, o, json.dumps(s, ensure_ascii=False)) for i, n, t, p, o, s in seed],
                )
                c.commit()

    # ---- users / sessions ----
    def user_count(self) -> int:
        with self.lock:
            return self.conn.execute("SELECT COUNT(*) FROM users").fetchone()[0]

    def create_user(self, username: str, password: str, role: str = "admin") -> int:
        salt = secrets.token_hex(16)
        h = hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt), n=2 ** 14, r=8, p=1, dklen=32).hex()
        with self.lock:
            cur = self.conn.execute(
                "INSERT INTO users(username,salt,hash,role,created) VALUES(?,?,?,?,?)",
                (username, salt, h, role, now()),
            )
            self.conn.commit()
            return int(cur.lastrowid)

    def verify_user(self, username: str, password: str):
        with self.lock:
            u = self.conn.execute("SELECT * FROM users WHERE username=?", (username,)).fetchone()
        if not u:
            return None
        h = hashlib.scrypt(password.encode(), salt=bytes.fromhex(u["salt"]), n=2 ** 14, r=8, p=1, dklen=32).hex()
        if not secrets.compare_digest(h, u["hash"]):
            return None
        return u

    def new_session(self, user_id: int, ip: str) -> str:
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.conn.execute(
                "INSERT INTO sessions(token,user_id,expires,ip) VALUES(?,?,?,?)",
                (token, user_id, now() + SESSION_TTL, ip),
            )
            self.conn.commit()
        return token

    def admin_user(self):
        with self.lock:
            return self.conn.execute("SELECT * FROM users WHERE role='admin' ORDER BY id LIMIT 1").fetchone()

    def create_api_token(self, user_id: int, name: str) -> str:
        raw = secrets.token_urlsafe(32)
        h = hashlib.sha256(raw.encode()).hexdigest()
        with self.lock:
            self.conn.execute(
                "INSERT INTO api_tokens(token_hash,user_id,name,created) VALUES(?,?,?,?)",
                (h, user_id, (name or "app")[:64], now()),
            )
            self.conn.commit()
        # Mirror into the C-side token store so the same token works for
        # C HTTP 18080 and WebSocket 2939 (strict JSON format).
        try:
            state = self.root / "state"
            state.mkdir(parents=True, exist_ok=True)
            tf = state / "tokens.json"
            arr = []
            if tf.is_file():
                try:
                    val = json.loads(tf.read_text(encoding="utf-8"))
                    if isinstance(val, list):
                        arr = val
                except Exception:
                    arr = []
            arr.append({"token": raw, "expires_at": now() + SESSION_TTL, "revoked": 0})
            arr = arr[-128:]
            tmp = state / "tokens.json.tmp"
            tmp.write_text(json.dumps(arr, ensure_ascii=False), encoding="utf-8")
            os.chmod(tmp, 0o600)
            os.replace(tmp, tf)
        except Exception as e:
            log("token mirror failed:", e)
        return raw

    def user_by_api_token(self, raw: str):
        if not raw or len(raw) > 200:
            return None
        h = hashlib.sha256(raw.encode()).hexdigest()
        with self.lock:
            r = self.conn.execute(
                "SELECT u.* FROM api_tokens t JOIN users u ON u.id=t.user_id "
                "WHERE t.token_hash=? AND t.revoked=0", (h,)
            ).fetchone()
        return r

    def session_user(self, token: str):
        if not token:
            return None
        with self.lock:
            s = self.conn.execute("SELECT * FROM sessions WHERE token=?", (token,)).fetchone()
            if not s or s["expires"] < now():
                return None
            return self.conn.execute("SELECT * FROM users WHERE id=?", (s["user_id"],)).fetchone()

    def drop_session(self, token: str) -> None:
        with self.lock:
            self.conn.execute("DELETE FROM sessions WHERE token=?", (token,))
            self.conn.commit()

    # ---- events ----
    def event(self, kind: str, detail: str) -> None:
        with self.lock:
            self.conn.execute("INSERT INTO events(ts,kind,detail) VALUES(?,?,?)", (now(), kind, detail[:500]))
            self.conn.commit()

    def events(self, limit: int = 100):
        with self.lock:
            rows = self.conn.execute(
                "SELECT id,ts,kind,detail FROM events ORDER BY id DESC LIMIT ?", (limit,)
            ).fetchall()
        return [{"id": r["id"], "ts": iso(r["ts"]), "kind": r["kind"], "detail": r["detail"]} for r in rows]

    # ---- devices ----
    def devices(self):
        with self.lock:
            rows = self.conn.execute("SELECT * FROM devices ORDER BY id").fetchall()
        out = []
        for r in rows:
            try:
                state = json.loads(r["state"])
            except Exception:
                state = {}
            out.append({
                "id": r["id"], "name": r["name"], "type": r["type"],
                "protocol": r["protocol"], "online": bool(r["online"]), "state": state,
            })
        return out

    def get_device(self, dev_id: str):
        with self.lock:
            r = self.conn.execute("SELECT * FROM devices WHERE id=?", (dev_id,)).fetchone()
        return self._device_row(r) if r else None

    def _device_row(self, r):
        try:
            state = json.loads(r["state"])
        except Exception:
            state = {}
        return {"id": r["id"], "name": r["name"], "type": r["type"],
                "protocol": r["protocol"], "online": bool(r["online"]), "state": state}

    def add_device(self, dev_id: str, name: str, dtype: str, protocol: str, online: int, state: dict):
        with self.lock:
            self.conn.execute(
                "INSERT OR REPLACE INTO devices(id,name,type,protocol,online,state) VALUES(?,?,?,?,?,?)",
                (dev_id, name, dtype, protocol, online, json.dumps(state, ensure_ascii=False)),
            )
            self.conn.commit()

    def update_device_state(self, dev_id: str, state: dict):
        with self.lock:
            self.conn.execute("UPDATE devices SET state=? WHERE id=?",
                              (json.dumps(state, ensure_ascii=False), dev_id))
            self.conn.commit()

    def delete_device(self, dev_id: str):
        with self.lock:
            self.conn.execute("DELETE FROM devices WHERE id=?", (dev_id,))
            self.conn.commit()

    # ---- settings / users ----
    def setting_get(self, key: str, default: str = "") -> str:
        with self.lock:
            r = self.conn.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        return r["value"] if r else default

    def setting_set(self, key: str, value: str) -> None:
        with self.lock:
            self.conn.execute(
                "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, str(value)),
            )
            self.conn.commit()

    def users(self):
        with self.lock:
            rows = self.conn.execute("SELECT id,username,role,created FROM users ORDER BY id").fetchall()
        return [{"id": r["id"], "username": r["username"], "role": r["role"], "created": iso(r["created"])} for r in rows]

    def delete_user(self, uid: int, keep_id: int) -> bool:
        with self.lock:
            r = self.conn.execute("SELECT id FROM users WHERE id=?", (uid,)).fetchone()
            if not r or uid == keep_id:
                return False
            self.conn.execute("DELETE FROM users WHERE id=?", (uid,))
            self.conn.execute("DELETE FROM sessions WHERE user_id=?", (uid,))
            self.conn.commit()
            return True

    # ---- ACL / files ----
    def acl_rows(self):
        with self.lock:
            rows = self.conn.execute("SELECT * FROM acl ORDER BY path,subject").fetchall()
        return [dict(r) for r in rows]

    def acl_set(self, subject: str, path: str, perm: str) -> None:
        with self.lock:
            self.conn.execute(
                "INSERT INTO acl(subject,path,perm) VALUES(?,?,?) "
                "ON CONFLICT(subject,path) DO UPDATE SET perm=excluded.perm",
                (subject, path, perm),
            )
            self.conn.commit()

    def acl_del(self, subject: str, path: str) -> None:
        with self.lock:
            self.conn.execute("DELETE FROM acl WHERE subject=? AND path=?", (subject, path))
            self.conn.commit()

    def acl_check(self, user, path: str, need: str) -> bool:
        if user is None:
            return False
        if user["role"] == "admin":
            return True
        subjects = [user["username"], user["role"]]
        best = -1
        perm = ""
        with self.lock:
            rows = self.conn.execute("SELECT subject,path,perm FROM acl").fetchall()
        for r in rows:
            if r["subject"] not in subjects:
                continue
            if path == r["path"] or path.startswith(r["path"].rstrip("/") + "/"):
                if len(r["path"]) > best:
                    best = len(r["path"])
                    perm = r["perm"]
        if best < 0:
            return False
        if perm == "rw":
            return need in ("read", "write")
        return perm == need or (need == "read" and perm in ("read", "rw"))

    def fs_path(self, rel: str) -> Path:
        rel = urllib.parse.unquote(rel or "/")
        if ".." in Path(rel).parts:
            raise ValueError("invalid path")
        p = (self.files / rel.lstrip("/")).resolve()
        if not str(p).startswith(str(self.files.resolve()) + os.sep) and p != self.files.resolve():
            raise ValueError("outside root")
        return p

    def fs_list(self, rel: str):
        p = self.fs_path(rel)
        if not p.exists() or not p.is_dir():
            return None
        out = []
        for e in sorted(p.iterdir(), key=lambda x: (x.is_file(), x.name.lower())):
            try:
                st = e.stat()
                out.append({
                    "name": e.name, "dir": e.is_dir(), "size": st.st_size,
                    "mtime": iso(st.st_mtime),
                    "rel": str(e.relative_to(self.files.resolve())),
                })
            except OSError:
                pass
        return out


def check_password(pw: str) -> bool:
    return isinstance(pw, str) and 8 <= len(pw) <= 200


class Handler(BaseHTTPRequestHandler):
    server_version = "LingOS-Web/" + VERSION
    store: Store
    web_dir: Path
    lan: bool = False
    pair_code: str = ""
    chat_sock: str = ""
    manager: DeviceManager | None = None
    pair_fails: dict = {}

    # ---- helpers ----
    def _json(self, code: int, obj, cookie: str = "") -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if cookie:
            self.send_header("Set-Cookie", f"{COOKIE}={cookie}; Path=/; Max-Age={SESSION_TTL}; HttpOnly; SameSite=Strict")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > MAX_BODY:
                return {}
            raw = self.rfile.read(n)
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def _cookie(self) -> str:
        raw = self.headers.get("Cookie", "")
        for part in raw.split(";"):
            k, _, v = part.strip().partition("=")
            if k == COOKIE:
                return v
        return ""

    def _user(self):
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            u = self.store.user_by_api_token(auth[7:].strip())
            if u:
                return u
        return self.store.session_user(self._cookie())

    def _root_ok(self) -> bool:
        expected = self.store.setting_get("root_key_hash", "")
        if not expected:
            return True   # legacy installs without a root key
        key = self.headers.get("X-Lingos-Root-Key", "")
        if not key:
            return False
        return secrets.compare_digest(hashlib.sha256(key.encode()).hexdigest(), expected)

    def _require_root(self) -> bool:
        if self._root_ok():
            return True
        self._json(403, {"error": "root key required", "code": "root_key_required"})
        return False

    def _require(self, role: str | None = None):
        u = self._user()
        if not u:
            self._json(401, {"error": "unauthorized"})
            return None
        if role and u["role"] != role:
            self._json(403, {"error": "forbidden"})
            return None
        return u

    def _local(self) -> bool:
        addr = self.client_address[0] if self.client_address else ""
        return addr in ("127.0.0.1", "::1", "localhost")

    def _pair_ok(self, body: dict) -> bool:
        if self._local():
            return True
        code = str(body.get("pair", "") or self.headers.get("X-Lingos-Pair", ""))
        if not self.pair_code:
            return False
        ip = self.client_address[0] if self.client_address else "?"
        hits = [t for t in Handler.pair_fails.get(ip, []) if time.time() - t < 60]
        if len(hits) >= 10:
            return False
        if secrets.compare_digest(code, self.pair_code):
            Handler.pair_fails.pop(ip, None)
            return True
        hits.append(time.time())
        Handler.pair_fails[ip] = hits
        return False

    def _origin_ok(self) -> bool:
        origin = self.headers.get("Origin")
        if not origin:
            return True
        host = self.headers.get("Host", "")
        try:
            return urllib.parse.urlparse(origin).netloc == host
        except Exception:
            return False

    def log_message(self, fmt, *args):
        log(self.client_address[0], fmt % args)

    # ---- HTTP verbs ----
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        path = u.path
        q = urllib.parse.parse_qs(u.query)
        if path == "/api/health":
            self._json(200, {
                "ok": True, "version": VERSION, "platform": sys.platform,
                "lan": self.lan, "setup": self.store.user_count() == 0,
                "time": iso(),
            })
            return
        if path == "/api/me":
            usr = self._user()
            if not usr:
                self._json(401, {"error": "unauthorized"})
                return
            self._json(200, {"username": usr["username"], "role": usr["role"]})
            return
        if path == "/api/apps":
            self._json(200, {"apps": [
                {"id": "home", "name": "概览", "icon": "home"},
                {"id": "devices", "name": "设备", "icon": "devices"},
                {"id": "files", "name": "文件", "icon": "files"},
                {"id": "security", "name": "安全", "icon": "security"},
                {"id": "ai", "name": "AI 助手", "icon": "ai"},
                {"id": "settings", "name": "设置", "icon": "settings"},
            ]})
            return
        if path == "/api/devices":
            if not self._require():
                return
            self._json(200, {"devices": self.store.devices(),
                             "adapters": self.manager.adapter_status() if self.manager else []})
            return
        if path == "/api/adapters":
            if not self._require():
                return
            self._json(200, {"adapters": self.manager.adapter_status() if self.manager else []})
            return
        if path == "/api/users":
            usr = self._require()
            if not usr:
                return
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            self._json(200, {"users": self.store.users()})
            return
        if path == "/api/settings":
            usr = self._require()
            if not usr:
                return
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            keys = ("ha_url", "ha_token", "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass", "mqtt_prefix", "miio_devices")
            self._json(200, {"settings": {k: self.store.setting_get(k, "") for k in keys}})
            return
        if path == "/api/events":
            if not self._require():
                return
            self._json(200, {"events": self.store.events()})
            return
        if path == "/api/acl":
            if not self._require():
                return
            self._json(200, {"acl": self.store.acl_rows()})
            return
        if path == "/api/security":
            if not self._require():
                return
            self._json(200, self._security_state())
            return
        if path == "/api/fs/list":
            usr = self._require()
            if not usr:
                return
            rel = (q.get("path") or ["/"])[0]
            try:
                if not self._fs_guard(usr, rel.rstrip("/") or "/", "read"):
                    self._json(403, {"error": "acl denied"})
                    return
                entries = self.store.fs_list(rel)
            except ValueError as e:
                self._json(400, {"error": str(e)})
                return
            if entries is None:
                self._json(404, {"error": "not found"})
                return
            self._json(200, {"path": rel, "entries": entries})
            return
        if path == "/api/fs/read":
            usr = self._require()
            if not usr:
                return
            rel = (q.get("path") or [""])[0]
            try:
                if not self._fs_guard(usr, rel, "read"):
                    self._json(403, {"error": "acl denied"})
                    return
                p = self.store.fs_path(rel)
                if not p.is_file() or p.stat().st_size > 2 * 1024 * 1024:
                    self._json(400, {"error": "not a readable file"})
                    return
                data = p.read_text(encoding="utf-8", errors="replace")
            except ValueError as e:
                self._json(400, {"error": str(e)})
                return
            self._json(200, {"path": rel, "content": data})
            return
        if path == "/api/audit/export":
            usr = self._require()
            if not usr:
                return
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            raw = json.dumps({"exported_at": iso(), "events": self.store.events(10000)},
                             ensure_ascii=False, indent=2).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Disposition", 'attachment; filename="lingos-audit.json"')
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return
        if path == "/api/backup":
            usr = self._require()
            if not usr:
                return
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            tmpdir = Path(tempfile.mkdtemp(prefix="lingos-backup-"))
            try:
                dbcopy = tmpdir / "webdesk.db"
                dst = sqlite3.connect(str(dbcopy))
                with self.store.lock:
                    self.store.conn.backup(dst)
                dst.close()
                archive = tmpdir / "lingos-backup.tar.gz"
                with tarfile.open(archive, "w:gz") as tar:
                    tar.add(dbcopy, arcname="webdesk.db")
                    if self.store.files.is_dir():
                        tar.add(self.store.files, arcname="files")
                    policy = self.store.root / "system" / "config" / "security-policy.json"
                    if policy.is_file():
                        tar.add(policy, arcname="security-policy.json")
                raw = archive.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/gzip")
                self.send_header("Content-Disposition", 'attachment; filename="lingos-backup.tar.gz"')
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
            except Exception as e:
                self._json(500, {"error": "backup failed", "detail": str(e)})
            finally:
                shutil.rmtree(tmpdir, ignore_errors=True)
            return
        self._static(path)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        path = u.path
        if not self._origin_ok():
            self._json(403, {"error": "bad origin"})
            return
        body = self._read_json()
        if path == "/api/setup":
            if self.store.user_count() > 0:
                self._json(409, {"error": "already initialized"})
                return
            if not self._pair_ok(body):
                self._json(403, {"error": "pair code required"})
                return
            username = str(body.get("username") or "admin").strip()
            pw = str(body.get("password") or "")
            if not username or len(username) > 64 or not check_password(pw):
                self._json(400, {"error": "username/password invalid (>=8 chars)"})
                return
            uid = self.store.create_user(username, pw, "admin")
            root_key = secrets.token_urlsafe(24)
            self.store.setting_set("root_key_hash", hashlib.sha256(root_key.encode()).hexdigest())
            token = self.store.new_session(uid, self.client_address[0])
            self.store.event("user", f"created admin {username}")
            self._json(200, {"ok": True, "username": username, "root_key": root_key}, cookie=token)
            return
        if path == "/api/login":
            username = str(body.get("username") or "")
            pw = str(body.get("password") or "")
            if not self._pair_ok(body):
                self._json(403, {"error": "pair code required"})
                return
            usr = self.store.verify_user(username, pw)
            if not usr:
                self._json(401, {"error": "invalid credentials"})
                return
            token = self.store.new_session(usr["id"], self.client_address[0])
            self.store.event("auth", f"login {username} from {self.client_address[0]}")
            self._json(200, {"ok": True, "username": usr["username"], "role": usr["role"]}, cookie=token)
            return
        if path == "/api/logout":
            self.store.drop_session(self._cookie())
            self.send_response(200)
            self.send_header("Set-Cookie", f"{COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/api/pair":
            if not (self.lan or self._local()):
                self._json(403, {"error": "pairing only on local/LAN"})
                return
            if not self._pair_ok(body):
                self._json(403, {"error": "pair code required"})
                return
            admin = self.store.admin_user()
            if not admin:
                self._json(409, {"error": "not initialized"})
                return
            token = self.store.create_api_token(admin["id"], "paired:" + str(body.get("name") or "device"))
            self.store.event("auth", "issued paired API token")
            self._json(200, {"ok": True, "token": token, "user": admin["username"], "role": admin["role"]})
            return
        usr = self._require()
        if not usr:
            return
        if path == "/api/rootkey":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            pw = str(body.get("password") or "")
            if not self.store.verify_user(usr["username"], pw):
                self._json(401, {"error": "invalid password"})
                return
            new_key = secrets.token_urlsafe(24)
            self.store.setting_set("root_key_hash", hashlib.sha256(new_key.encode()).hexdigest())
            self.store.event("security", "root key rotated")
            self._json(200, {"ok": True, "root_key": new_key})
            return
        if path == "/api/token":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            raw = self.store.create_api_token(usr["id"], str(body.get("name") or "app"))
            self.store.event("auth", "issued API token")
            self._json(200, {"ok": True, "token": raw, "user": usr["username"]})
            return
        if path == "/api/cmd":
            # App/Web same rights: authenticated Web/API-token callers reach the
            # C command backend through loopback only. Never expose 18080 directly.
            backend = os.environ.get("LINGOS_BACKEND_HTTP", "http://127.0.0.1:18080").rstrip("/")
            try:
                data = json.dumps(body, ensure_ascii=False).encode("utf-8")
                req = urllib.request.Request(backend + "/api/cmd", data=data, method="POST",
                                             headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=20) as rr:
                    raw = rr.read()
                    code = rr.status
                self.send_response(code)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
            except Exception as e:
                self._json(502, {"error": "backend unavailable", "detail": str(e)})
            return
        if path == "/api/devices":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            name = str(body.get("name") or "虚拟设备").strip()[:64]
            dtype = str(body.get("type") or "switch").strip()
            r = self.manager.add_virtual(name, dtype) if self.manager else {"id": ""}
            self._json(200, {"ok": True, "device": r})
            return
        if path == "/api/devices/discover":
            r = self.manager.discover(str(body.get("protocol") or "")) if self.manager else []
            self._json(200, {"devices": r, "count": len(r)})
            return
        if path.startswith("/api/devices/") and path.endswith("/command"):
            dev_id = path[len("/api/devices/"):-len("/command")].strip("/")
            r = self.manager.command(dev_id, str(body.get("cmd") or ""), body.get("args") or {}) if self.manager else None
            if r is None:
                self._json(404, {"error": "device not found"})
                return
            self._json(200 if r.get("ok") else 400, {"ok": bool(r.get("ok")), "device": r})
            return
        if path == "/api/users":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            uname = str(body.get("username") or "").strip()
            pw = str(body.get("password") or "")
            role = str(body.get("role") or "user")
            if not uname or len(uname) > 64 or not check_password(pw) or role not in ("admin", "user"):
                self._json(400, {"error": "username/password/role invalid"})
                return
            try:
                uid = self.store.create_user(uname, pw, role)
            except Exception as e:
                self._json(409, {"error": "user exists: " + str(e)})
                return
            self.store.event("user", f"created {role} {uname}")
            self._json(200, {"ok": True, "id": uid})
            return
        if path == "/api/settings":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            allowed = ("ha_url", "ha_token", "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass", "mqtt_prefix", "miio_devices")
            for k, v in (body or {}).items():
                if k in allowed:
                    self.store.setting_set(k, str(v))
            self.store.event("settings", "updated adapters")
            self._json(200, {"ok": True})
            return
        if path == "/api/fs/write":
            rel = str(body.get("path") or "")
            content = str(body.get("content") or "")
            if len(content.encode("utf-8")) > 2 * 1024 * 1024:
                self._json(400, {"error": "content too large"})
                return
            try:
                if not self._fs_guard(usr, rel, "write"):
                    self._json(403, {"error": "acl denied"})
                    return
                p = self.store.fs_path(rel)
                if str(p).startswith("/LINGOS/bin") or str(p).startswith("/LINGOS/system"):
                    self._json(403, {"error": "protected path"})
                    return
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")
            except ValueError as e:
                self._json(400, {"error": str(e)})
                return
            self.store.event("fs", f"write {rel} by {usr['username']}")
            self._json(200, {"ok": True})
            return
        if path == "/api/acl":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            if not self._require_root():
                return
            subject = str(body.get("subject") or "").strip()
            apath = str(body.get("path") or "/").strip() or "/"
            perm = str(body.get("perm") or "read")
            if not subject or perm not in ("read", "write", "rw"):
                self._json(400, {"error": "subject/perm invalid"})
                return
            if ".." in Path(apath).parts:
                self._json(400, {"error": "invalid path"})
                return
            self.store.acl_set(subject, apath, perm)
            self.store.event("acl", f"{subject} {apath} {perm}")
            self._json(200, {"ok": True})
            return
        if path == "/api/security/level":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            level = str(body.get("level") or "L1")
            if level not in LEVELS:
                self._json(400, {"error": "invalid level"})
                return
            sp = self.store.root / "system" / "config" / "security-policy.json"
            sp.parent.mkdir(parents=True, exist_ok=True)
            sp.write_text(json.dumps({"schema_version": 1, "privacy_level": level, "local_only": True, "flags": {}}, ensure_ascii=False), encoding="utf-8")
            try:
                os.chmod(sp, 0o644)
            except OSError:
                pass
            self.store.event("security", f"level={level}")
            self._json(200, {"ok": True, "level": level})
            return
        if path == "/api/chat":
            text = str(body.get("text") or "").strip()
            if not text:
                self._json(400, {"error": "empty"})
                return
            reply = self._chat(text)
            self._json(200, {"reply": reply})
            return
        self._json(404, {"error": "not found"})

    def do_DELETE(self):
        u = urllib.parse.urlparse(self.path)
        if not self._origin_ok():
            self._json(403, {"error": "bad origin"})
            return
        usr = self._require()
        if not usr:
            return
        if u.path == "/api/fs":
            rel = (urllib.parse.parse_qs(u.query).get("path") or [""])[0]
            if not rel:
                self._json(400, {"error": "path required"})
                return
            try:
                if not self._fs_guard(usr, rel, "write"):
                    self._json(403, {"error": "acl denied"})
                    return
                p = self.store.fs_path(rel)
                if p.is_dir():
                    self._json(400, {"error": "directory delete not enabled"})
                    return
                p.unlink(missing_ok=True)
            except ValueError as e:
                self._json(400, {"error": str(e)})
                return
            self.store.event("fs", f"delete {rel} by {usr['username']}")
            self._json(200, {"ok": True})
            return
        if u.path == "/api/acl":
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            if not self._require_root():
                return
            q = urllib.parse.parse_qs(u.query)
            self.store.acl_del((q.get("subject") or [""])[0], (q.get("path") or [""])[0])
            self._json(200, {"ok": True})
            return
        if u.path.startswith("/api/devices/") and self.manager:
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            if not self._require_root():
                return
            self.manager.remove(u.path[len("/api/devices/"):].strip("/"))
            self._json(200, {"ok": True})
            return
        if u.path.startswith("/api/users/"):
            if usr["role"] != "admin":
                self._json(403, {"error": "admin required"})
                return
            if not self._require_root():
                return
            try:
                uid = int(u.path[len("/api/users/"):].strip("/"))
            except ValueError:
                self._json(400, {"error": "bad user id"})
                return
            if not self.store.delete_user(uid, usr["id"]):
                self._json(400, {"error": "cannot delete this user"})
                return
            self._json(200, {"ok": True})
            return
        self._json(404, {"error": "not found"})

    # ---- internals ----
    def _level(self) -> str:
        sp = self.store.root / "system" / "config" / "security-policy.json"
        try:
            return str(json.loads(sp.read_text())["privacy_level"])
        except Exception:
            return "L1"

    def _fs_guard(self, usr, rel: str, need: str) -> bool:
        # L3/L4: non-admin file access is denied before ACL.
        if self._level() in ("L3", "L4") and usr["role"] != "admin":
            return False
        return self.store.acl_check(usr, rel, need)

    def _security_state(self):
        sp = self.store.root / "system" / "config" / "security-policy.json"
        level = "L1"
        try:
            level = json.loads(sp.read_text())["privacy_level"]
        except Exception:
            pass
        name, desc = LEVELS.get(level, LEVELS["L1"])
        return {
            "level": level, "name": name, "description": desc,
            "levels": [{"id": k, "name": v[0], "description": v[1]} for k, v in LEVELS.items()],
            "platform": "termux" if "com.termux" in os.environ.get("PREFIX", "") else sys.platform,
            "audit": self.store.events(30),
        }

    def _chat(self, text: str) -> str:
        sock_path = self.chat_sock or str(self.store.root / "run" / "ai.sock")
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(8)
            s.connect(sock_path)
            s.sendall((json.dumps({"cmd": "chat", "params": {"text": text}}, ensure_ascii=False) + "\n").encode())
            data = b""
            while len(data) < 2 * 1024 * 1024:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
                if b"\n" in chunk:
                    break
            s.close()
            obj = json.loads(data.decode("utf-8", "replace").strip().splitlines()[0])
            if isinstance(obj, dict):
                for k in ("reply", "content", "message", "text"):
                    if isinstance(obj.get(k), str) and obj[k].strip():
                        return obj[k]
                if obj.get("ok") is False:
                    return "AI 服务返回错误：" + str(obj.get("error") or obj)[:300]
            return json.dumps(obj, ensure_ascii=False)[:2000]
        except Exception as e:
            return f"AI 服务暂未连接（{e}）。Web 桌面已可用；启动 lingos 后端后会接入。"

    def _static(self, path: str):
        if path.startswith("/api/"):
            self._json(404, {"error": "not found"})
            return
        if path in ("/", ""):
            path = "/index.html"
        rel = path.lstrip("/")
        if ".." in Path(rel).parts:
            self._json(400, {"error": "bad path"})
            return
        p = (self.web_dir / rel).resolve()
        if not str(p).startswith(str(self.web_dir.resolve())):
            self._json(403, {"error": "forbidden"})
            return
        if not p.is_file():
            p = self.web_dir / "index.html"
        if not p.is_file():
            self._json(404, {"error": "webdesk missing"})
            return
        ctype = {
            ".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
            ".css": "text/css; charset=utf-8", ".json": "application/json; charset=utf-8",
            ".webmanifest": "application/manifest+json", ".svg": "image/svg+xml",
            ".png": "image/png", ".ico": "image/x-icon",
        }.get(p.suffix.lower(), "application/octet-stream")
        data = p.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        if p.name == "sw.js":
            self.send_header("Cache-Control", "no-cache")
        else:
            self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.environ.get("LINGOS_ROOT", "/LINGOS"))
    ap.add_argument("--web", default="")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--lan", action="store_true")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    web = Path(args.web).resolve() if args.web else (Path(__file__).resolve().parents[2] / "webdesk")
    if not web.is_dir():
        web = root / "webdesk"
    if not web.is_dir():
        log("FATAL: webdesk directory not found:", web)
        return 2
    store = Store(root)
    Handler.store = store
    Handler.manager = DeviceManager(store)
    Handler.web_dir = web
    Handler.lan = args.lan
    Handler.pair_code = "".join(secrets.choice("0123456789") for _ in range(6)) if args.lan else ""
    host = "0.0.0.0" if args.lan else "127.0.0.1"
    httpd = ThreadingHTTPServer((host, args.port), Handler)
    httpd.daemon_threads = True
    log(f"LING OS {VERSION} Web Desktop on http://{host}:{args.port} (root={root}, web={web})")
    if args.lan and Handler.pair_code:
        log("LAN mode: pair code =", Handler.pair_code)
        log("Use it for the first setup/login from another device.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("stopping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
