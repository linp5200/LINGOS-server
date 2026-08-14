#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LING OS Git Skills（只读：status/log/diff）
版本: LN-B-5.0.0.0-rc0.4
功能：AI 查看 git 仓库状态/历史/差异（不执行任何写操作）
安全：只读命令 + 超时 + 路径校验（拒绝根目录/系统关键目录）
核心协议：C1（容错/日志）、C-C（容错）
"""

import os
import subprocess
import logging

logger = logging.getLogger("GitSkills")

GIT_TIMEOUT = 10
# 禁止作为仓库根目录的路径（防止误操作/越权）
FORBIDDEN_ROOTS = ("/", "/LINGOS", "/etc", "/usr", "/var", "/bin", "/sbin",
                   "/home", "/root", "/tmp")


def _run_git(cwd, args):
    """执行只读 git 命令（超时保护）"""
    try:
        if not cwd or not os.path.isdir(cwd):
            return False, "Error: invalid directory"
        # 路径校验：禁止系统关键目录
        real = os.path.realpath(cwd)
        for forbid in FORBIDDEN_ROOTS:
            if real == forbid or real.startswith(forbid + "/"):
                return False, "Error: directory not allowed"
        result = subprocess.run(
            ["git", "-C", real] + args,
            capture_output=True, text=True, timeout=GIT_TIMEOUT
        )
        if result.returncode != 0:
            return False, (result.stderr or "git error").strip()[:1000]
        return True, result.stdout.strip()[:4000]
    except subprocess.TimeoutExpired:
        return False, "Error: git command timed out"
    except Exception as e:
        return False, f"Error: {e}"


def git_status(args_json: str):
    """查看 git 仓库状态（--short）

    参数: {"path": "仓库目录"}
    """
    try:
        import json
        args = json.loads(args_json)
        path = args.get("path", ".")
        return _run_git(path, ["status", "--short"])
    except Exception as e:
        return False, str(e)


def git_log(args_json: str):
    """查看 git 提交历史（--oneline，默认 20 条）

    参数: {"path": "仓库目录", "count": 条数(默认20)}
    """
    try:
        import json
        args = json.loads(args_json)
        path = args.get("path", ".")
        count = min(int(args.get("count", 20)), 100)
        return _run_git(path, ["log", "--oneline", "-n", str(count)])
    except Exception as e:
        return False, str(e)


def git_diff(args_json: str):
    """查看 git 未提交差异（--stat 摘要 + 详情可选）

    参数: {"path": "仓库目录", "detail": true/false}
    """
    try:
        import json
        args = json.loads(args_json)
        path = args.get("path", ".")
        detail = args.get("detail", False)
        if detail:
            return _run_git(path, ["diff"])
        return _run_git(path, ["diff", "--stat"])
    except Exception as e:
        return False, str(e)
