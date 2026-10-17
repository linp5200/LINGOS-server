#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS 用户管理（本地账户 + 角色 + PBKDF2 密码）

存储：/LINGOS/system/config/users.json（0600）
角色：owner / admin / user / guest
安全：
  - 密码只存 PBKDF2-HMAC-SHA256(盐 + 120000 迭代)，绝不落明文
  - 原子写（tmp + os.replace）
  - 至少保留一个启用的 owner；默认 owner 首次启动自动创建
命令面（ext_dispatch）：user_list / user_add / user_update / user_set_role /
                      user_reset_password / user_delete / user_stats
"""
import hashlib
import hmac
import json
import os
import re
import secrets
import threading
import time
from typing import Any, Dict, List, Optional

try:
    from data_path import lingos_data_root  # type: ignore
except Exception:
    def lingos_data_root() -> str:
        return os.environ.get("LINGOS_ROOT", "/LINGOS")


LOCK = threading.RLock()
ROLES = ("owner", "admin", "user", "guest")
USERNAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]{2,31}$")
ITERATIONS = 120000


def _users_path() -> str:
    return os.path.join(lingos_data_root(), "system", "config", "users.json")


def _now() -> int:
    return int(time.time())


def __empty() -> Dict[str, Any]:
    return {"version": 1, "users": []}


def _load() -> Dict[str, Any]:
    p = _users_path()
    try:
        with open(p, "r", encoding="utf-8") as f:
            d = json.load(f)
        if not isinstance(d, dict) or not isinstance(d.get("users"), list):
            return __empty()
        return d
    except FileNotFoundError:
        return __empty()
    except Exception:
        return __empty()


def _save(d: Dict[str, Any]) -> None:
    p = _users_path()
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    try:
        os.chmod(tmp, 0o600)
    except Exception:
        pass
    os.replace(tmp, p)


def _public(u: Dict[str, Any]) -> Dict[str, Any]:
    out = {k: v for k, v in u.items() if k != "pass_hash"}
    return out


def _find(d: Dict[str, Any], username: str) -> Optional[Dict[str, Any]]:
    for u in d.get("users", []):
        if u.get("username") == username:
            return u
    return None


def _hash_password(password: str, salt: Optional[bytes] = None) -> str:
    if salt is None:
        salt = secrets.token_bytes(16)
    h = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, ITERATIONS)
    return "pbkdf2_sha256$%d$%s$%s" % (ITERATIONS, salt.hex(), h.hex())


def _check_password(stored: str, password: str) -> bool:
    try:
        algo, iters, salt_hex, hash_hex = stored.split("$")
        if algo != "pbkdf2_sha256":
            return False
        h = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                                bytes.fromhex(salt_hex), int(iters))
        return hmac.compare_digest(h.hex(), hash_hex)
    except Exception:
        return False


def _validate(username: str, password: str) -> None:
    if not USERNAME_RE.match(username or ""):
        raise ValueError("用户名需 3-32 位，字母开头，仅字母数字_.-")
    if not password or len(password) < 8:
        raise ValueError("密码至少 8 位")
    if len(password) > 256:
        raise ValueError("密码过长")


def ensure_default_owner() -> Dict[str, Any]:
    """首次启动创建默认 owner：admin / lingos（必须修改）。

    可用环境变量 LINGOS_ADMIN_PASSWORD 指定初始密码。
    返回 {created: bool, username, default_password?}。
    """
    with LOCK:
        d = _load()
        owners = [u for u in d.get("users", [])
                  if u.get("role") == "owner" and u.get("enabled", True)]
        if owners:
            return {"created": False, "username": owners[0]["username"]}
        pwd = os.environ.get("LINGOS_ADMIN_PASSWORD", "lingos")
        u = {
            "id": "u_" + secrets.token_hex(8),
            "username": "admin",
            "display_name": "管理员",
            "role": "owner",
            "enabled": True,
            "must_change_password": True,
            "pass_hash": _hash_password(pwd),
            "created": _now(),
            "last_login": 0,
        }
        d.setdefault("users", []).append(u)
        _save(d)
        return {"created": True, "username": "admin",
                "default_password": pwd, "must_change_password": True}


def list_users() -> Dict[str, Any]:
    with LOCK:
        d = _load()
        arr = [_public(u) for u in d.get("users", [])]
        arr.sort(key=lambda x: (ROLES.index(x.get("role", "user")) if x.get("role") in ROLES else 99,
                                x.get("username", "")))
        return {"status": "ok", "count": len(arr), "data": arr,
                "roles": list(ROLES)}


def add_user(username: str = "", password: str = "", role: str = "user",
             display_name: str = "") -> Dict[str, Any]:
    try:
        username = (username or "").strip()
        display_name = (display_name or "").strip()[:64]
        role = (role or "user").strip()
        _validate(username, password or "")
    except ValueError as e:
        return {"status": "error", "msg": str(e)}
    if role not in ROLES:
        return {"status": "error", "msg": "角色必须是 " + "/".join(ROLES)}
    with LOCK:
        d = _load()
        if _find(d, username):
            return {"status": "error", "msg": "用户名已存在"}
        u = {
            "id": "u_" + secrets.token_hex(8),
            "username": username,
            "display_name": display_name or username,
            "role": role,
            "enabled": True,
            "must_change_password": False,
            "pass_hash": _hash_password(password),
            "created": _now(),
            "last_login": 0,
        }
        d.setdefault("users", []).append(u)
        _save(d)
        return {"status": "ok", "data": _public(u)}


def update_user(username: str = "", display_name: Optional[str] = None,
                enabled: Optional[bool] = None) -> Dict[str, Any]:
    with LOCK:
        d = _load()
        u = _find(d, (username or "").strip())
        if not u:
            return {"status": "error", "msg": "用户不存在"}
        if enabled is False and u.get("role") == "owner":
            owners = [x for x in d["users"]
                      if x.get("role") == "owner" and x.get("enabled", True)]
            if len(owners) <= 1:
                return {"status": "error", "msg": "不能禁用最后一个 owner"}
        if display_name is not None:
            u["display_name"] = str(display_name)[:64]
        if enabled is not None:
            u["enabled"] = bool(enabled)
        u["updated"] = _now()
        _save(d)
        return {"status": "ok", "data": _public(u)}


def set_role(username: str = "", role: str = "") -> Dict[str, Any]:
    role = (role or "").strip()
    if role not in ROLES:
        return {"status": "error", "msg": "角色必须是 " + "/".join(ROLES)}
    with LOCK:
        d = _load()
        u = _find(d, (username or "").strip())
        if not u:
            return {"status": "error", "msg": "用户不存在"}
        if u.get("role") == "owner" and role != "owner":
            owners = [x for x in d["users"]
                      if x.get("role") == "owner" and x.get("enabled", True)]
            if len(owners) <= 1:
                return {"status": "error", "msg": "不能降级最后一个 owner"}
        u["role"] = role
        u["updated"] = _now()
        _save(d)
        return {"status": "ok", "data": _public(u)}


def set_password(username: str = "", password: str = "",
                 must_change: bool = False) -> Dict[str, Any]:
    try:
        _validate("placeholder", password or "")
    except ValueError as e:
        return {"status": "error", "msg": str(e)}
    with LOCK:
        d = _load()
        u = _find(d, (username or "").strip())
        if not u:
            return {"status": "error", "msg": "用户不存在"}
        u["pass_hash"] = _hash_password(password)
        u["must_change_password"] = bool(must_change)
        u["updated"] = _now()
        _save(d)
        return {"status": "ok", "msg": "密码已更新"}


def verify_user(username: str = "", password: str = "") -> Optional[Dict[str, Any]]:
    with LOCK:
        d = _load()
        u = _find(d, (username or "").strip())
        if not u or not u.get("enabled", True):
            return None
        if not _check_password(u.get("pass_hash", ""), password or ""):
            return None
        u["last_login"] = _now()
        _save(d)
        return _public(u)


def delete_user(username: str = "") -> Dict[str, Any]:
    with LOCK:
        d = _load()
        u = _find(d, (username or "").strip())
        if not u:
            return {"status": "error", "msg": "用户不存在"}
        if u.get("role") == "owner":
            owners = [x for x in d["users"]
                      if x.get("role") == "owner" and x.get("enabled", True)]
            if len(owners) <= 1:
                return {"status": "error", "msg": "不能删除最后一个 owner"}
        d["users"] = [x for x in d["users"] if x.get("username") != u["username"]]
        _save(d)
        return {"status": "ok", "msg": "已删除", "username": u["username"]}


# ============================================================
# ext_dispatch 命令入口
# ============================================================

def cmd_user_list() -> Dict[str, Any]:
    ensure_default_owner()
    return list_users()


def cmd_user_stats() -> Dict[str, Any]:
    with LOCK:
        d = _load()
        users = d.get("users", [])
        by_role = {r: 0 for r in ROLES}
        for u in users:
            r = u.get("role", "user")
            if r in by_role:
                by_role[r] += 1
        return {"status": "ok", "data": {
            "total": len(users),
            "enabled": sum(1 for u in users if u.get("enabled", True)),
            "by_role": by_role,
            "storage": _users_path(),
        }}


def cmd_user_add(username: str = "", password: str = "", role: str = "user",
                 display_name: str = "") -> Dict[str, Any]:
    ensure_default_owner()
    return add_user(username, password, role, display_name)


def cmd_user_update(username: str = "", display_name: Optional[str] = None,
                    enabled: Optional[bool] = None) -> Dict[str, Any]:
    if isinstance(enabled, str):
        enabled = enabled.strip().lower() in ("1", "true", "yes", "on")
    return update_user(username, display_name, enabled)


def cmd_user_set_role(username: str = "", role: str = "") -> Dict[str, Any]:
    return set_role(username, role)


def cmd_user_reset_password(username: str = "", password: str = "") -> Dict[str, Any]:
    return set_password(username, password, must_change=True)


def cmd_user_delete(username: str = "") -> Dict[str, Any]:
    return delete_user(username)


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print(json.dumps({"usage": "user_manager.py list | add <u> <p> [role] | delete <u>"},
                         ensure_ascii=False))
        sys.exit(0)
    cmd = sys.argv[1]
    if cmd == "list":
        print(json.dumps(cmd_user_list(), ensure_ascii=False, indent=2))
    elif cmd == "stats":
        print(json.dumps(cmd_user_stats(), ensure_ascii=False, indent=2))
    elif cmd == "add" and len(sys.argv) >= 4:
        print(json.dumps(cmd_user_add(sys.argv[2], sys.argv[3],
                                      sys.argv[4] if len(sys.argv) > 4 else "user"),
                         ensure_ascii=False, indent=2))
    elif cmd == "delete" and len(sys.argv) >= 3:
        print(json.dumps(cmd_user_delete(sys.argv[2]), ensure_ascii=False, indent=2))
    else:
        print("usage error")
