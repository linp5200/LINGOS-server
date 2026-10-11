#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LING OS Sub-AI Worker（【2026-10-11 P5】补齐：被依赖检查/进程表引用但此前缺失）

本文件是**真实可用**的实现，两个职责：

  ① 孤儿任务回收 recover_orphaned_tasks()
     进程崩溃后 /LINGOS/state/tasks.json 中遗留的 running 任务（owner pid 已消失）
     置为 failed——由 sub_ai_scheduler.SubAIScheduler.__init__ 在启动时调用。

  ② 单任务 worker 进程
     python3 sub_ai_worker.py --task-id subtask_...   # 由 SubAIScheduler._spawn_worker 拉起
     python3 sub_ai_worker.py --prompt "..." --role general
     任务执行复用 sub_ai_scheduler.react_loop（不复制实现）；
     日志 /LINGOS/Debug/sub_ai_worker_<task_id>.log（与 src/fs/sync_template.c 排障指引一致）。

引用方（真实接线，非占位）：
  · src/core/dependency_check.c:66  要求 /LINGOS/bin/sub_ai_worker.py 存在
  · src/shell/system_info.c:104     进程表中查找 sub_ai_worker.py
  · src/fs/sync_template.c:183      指引 tail -f /LINGOS/Debug/sub_ai_worker_*.log
  · src/python/sub_ai_scheduler.py  SubAIScheduler.__init__ / _spawn_worker

依赖方向：本模块顶层仅用标准库；sub_ai_scheduler 在函数内延迟导入（避免循环导入）。
"""

import os
import sys
import json
import time
import fcntl
import logging
import argparse
import tempfile
from typing import Dict, List, Optional, Tuple

TASKS_STATE_PATH = "/LINGOS/state/tasks.json"
LOCK_PATH = TASKS_STATE_PATH + ".lock"
LOG_DIR = "/LINGOS/Debug"
CONFIG_PATH = "/LINGOS/system/config/ai_config.json"

logger = logging.getLogger("SubAIWorker")

# ========== 多语言（与其它 Python 模块同口径） ==========
_current_lang = "en"


def t(en: str, zh: str) -> str:
    return zh if _current_lang == "zh" else en


def load_language_preference() -> None:
    global _current_lang
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                lang = (json.load(f) or {}).get("language", "en")
            if lang in ("en", "zh"):
                _current_lang = lang
    except Exception:
        pass


# =============================================================
# 任务状态文件（读-改-写；flock 串行化 + 原子替换）
# =============================================================

def _load_state() -> Dict:
    """读 tasks.json → {"tasks": [...]}；缺失/损坏返回空结构"""
    if not os.path.exists(TASKS_STATE_PATH):
        return {"tasks": []}
    try:
        with open(TASKS_STATE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and isinstance(data.get("tasks"), list):
            return data
        return {"tasks": []}
    except Exception as e:
        logger.error("load tasks state failed: %s", e)
        return {"tasks": []}


def _write_state_locked(state: Dict) -> None:
    """在 flock 保护下原子写回 tasks.json（0600）"""
    os.makedirs(os.path.dirname(TASKS_STATE_PATH), exist_ok=True)
    lock_fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(TASKS_STATE_PATH), prefix=".tasks_")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(state, f, indent=2, ensure_ascii=False)
            os.chmod(tmp, 0o600)
            os.replace(tmp, TASKS_STATE_PATH)
        except Exception:
            try:
                os.remove(tmp)
            except Exception:
                pass
            raise
    finally:
        try:
            os.close(lock_fd)
        except Exception:
            pass


def _find_task(task_id: str) -> Optional[Dict]:
    """按 task_id 取任务记录（返回副本）"""
    for rec in _load_state().get("tasks", []):
        if isinstance(rec, dict) and rec.get("task_id") == task_id:
            return dict(rec)
    return None


def _update_task(task_id: str, **fields) -> bool:
    """读-改-写单个任务字段（跨进程 flock 串行化）"""
    os.makedirs(os.path.dirname(TASKS_STATE_PATH), exist_ok=True)
    lock_fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        state = _load_state()
        found = False
        for rec in state.get("tasks", []):
            if isinstance(rec, dict) and rec.get("task_id") == task_id:
                rec.update(fields)
                found = True
                break
        if not found:
            return False
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(TASKS_STATE_PATH), prefix=".tasks_")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(state, f, indent=2, ensure_ascii=False)
            os.chmod(tmp, 0o600)
            os.replace(tmp, TASKS_STATE_PATH)
        except Exception:
            try:
                os.remove(tmp)
            except Exception:
                pass
            raise
        return True
    except Exception as e:
        logger.error("update task %s failed: %s", task_id, e)
        return False
    finally:
        try:
            os.close(lock_fd)
        except Exception:
            pass


# =============================================================
# ① 孤儿任务回收
# =============================================================

def _get(task, key, default=None):
    if isinstance(task, dict):
        return task.get(key, default)
    return getattr(task, key, default)


def _set(task, key, value) -> None:
    if isinstance(task, dict):
        task[key] = value
    else:
        setattr(task, key, value)


def _pid_alive(pid) -> bool:
    """pid 是否仍存在（None/非法 = 不存活）"""
    try:
        pid = int(pid)
    except Exception:
        return False
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except Exception:
        return False
    return True


def recover_orphaned_tasks(tasks: Dict, save_fn=None) -> int:
    """把悬挂的 running 任务置为 failed，返回回收数量

    判定：status == "running" 且 owner pid 不存活（缺失同样视为不存活——
    旧版本记录无 owner_pid，而当前进程刚启动，不可能由它执行）。
    注意：PID 复用/zombie 未回收时可能误判为存活（宁可漏回收，不误杀）。
    """
    if not isinstance(tasks, dict) or not tasks:
        return 0
    now = time.time()
    recovered = 0
    for tid, task in list(tasks.items()):
        if _get(task, "status") != "running":
            continue
        pid = _get(task, "owner_pid")
        if _pid_alive(pid):
            continue
        _set(task, "status", "failed")
        _set(task, "error", "orphaned: worker process (pid=%s) is gone" % (pid,))
        _set(task, "completed_at", now)
        _set(task, "updated_at", now)
        recovered += 1
        logger.warning("task %s recovered as failed (orphaned, pid=%s)", tid, pid)
    if recovered and save_fn is not None:
        try:
            save_fn(tasks)
        except Exception as e:
            logger.error("persist recovered tasks failed: %s", e)
    return recovered


# =============================================================
# ② 单任务执行
# =============================================================

def run_task(task_id: str) -> Tuple[bool, str]:
    """在**本进程**执行一个已登记的子AI任务（供 worker 进程启动时调用）"""
    rec = _find_task(task_id)
    if not rec:
        return False, t("task not found: %s" % task_id, "任务不存在：%s" % task_id)
    prompt = str(rec.get("prompt") or "")
    role = str(rec.get("role") or "general")
    if not prompt:
        _update_task(task_id, status="failed", error="empty prompt",
                     completed_at=time.time(), updated_at=time.time())
        return False, t("task has empty prompt", "任务提示为空")

    now = time.time()
    _update_task(task_id, status="running", owner_pid=os.getpid(),
                 started_at=now, updated_at=now, error=None)
    try:
        from sub_ai_scheduler import react_loop
        result = react_loop(prompt, role=role)
    except Exception as e:
        _update_task(task_id, status="failed", error=str(e),
                     completed_at=time.time(), updated_at=time.time())
        return False, str(e)

    ok = isinstance(result, str) and not result.strip().startswith(
        ("AI Error:", "Error:", "Maximum iterations reached"))
    if ok:
        _update_task(task_id, status="done", result=result, error=None,
                     completed_at=time.time(), updated_at=time.time())
    else:
        _update_task(task_id, status="failed", error=str(result),
                     completed_at=time.time(), updated_at=time.time())
    return ok, str(result)


def run_prompt(prompt: str, role: str = "general") -> Tuple[bool, str]:
    """一次性任务（不写任务板）——供 CLI/人工使用"""
    try:
        from sub_ai_scheduler import react_loop
        result = react_loop(prompt, role=role)
    except Exception as e:
        return False, str(e)
    ok = isinstance(result, str) and not result.strip().startswith(
        ("AI Error:", "Error:", "Maximum iterations reached"))
    return ok, str(result)


# =============================================================
# CLI
# =============================================================

def main(argv: Optional[List[str]] = None) -> int:
    load_language_preference()
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s")
    parser = argparse.ArgumentParser(
        prog="sub_ai_worker.py",
        description=t("LINGOS sub-AI worker (single task process / orphan recovery)",
                      "LINGOS 子AI worker（单任务进程 / 孤儿任务回收）"))
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--task-id", help=t("run a registered task by id", "按 id 执行已登记任务"))
    group.add_argument("--prompt", help=t("run a one-shot prompt", "执行一次性提示"))
    group.add_argument("--recover", action="store_true",
                       help=t("recover orphaned running tasks", "回收悬挂的 running 任务"))
    group.add_argument("--list", action="store_true", help=t("list task board", "列出任务板"))
    parser.add_argument("--role", default="general", help=t("sub-AI role", "子AI 角色"))
    args = parser.parse_args(argv)

    if args.recover:
        state = _load_state()
        tasks = {r.get("task_id"): r for r in state.get("tasks", []) if isinstance(r, dict)}
        n = recover_orphaned_tasks(tasks, save_fn=_persist_task_map)
        print(json.dumps({"status": "ok", "recovered": n}, ensure_ascii=False))
        return 0

    if args.list:
        state = _load_state()
        rows = []
        for r in state.get("tasks", []):
            if not isinstance(r, dict):
                continue
            rows.append({k: r.get(k) for k in
                         ("task_id", "status", "role", "owner_pid", "created_at", "updated_at")})
        print(json.dumps({"status": "ok", "count": len(rows), "tasks": rows},
                         ensure_ascii=False, indent=2))
        return 0

    if args.task_id:
        ok, out = run_task(args.task_id)
    else:
        ok, out = run_prompt(args.prompt, role=args.role)
    print(json.dumps({"status": "ok" if ok else "error", "result": out},
                     ensure_ascii=False))
    return 0 if ok else 1


def _persist_task_map(tasks: Dict) -> None:
    """recover CLI 的持久化回调：把 {task_id: record} 写回 tasks.json"""
    _write_state_locked({"tasks": list(tasks.values())})


if __name__ == "__main__":
    sys.exit(main())
