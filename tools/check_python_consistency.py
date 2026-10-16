#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LINGOS Python 侧一致性校验（清单 ↔ 实际 handler）——task-10 ③ 枯燥活脚本化

把"清单里声明的技能/命令"与"真实存在的 handler"做**静态**比对，防止再次混入
幽灵引用与半成品（历史事故：alert_query 引用不存在的 alert_manager.py、
sub_ai_worker.py 被依赖检查引用却不存在、register_skill 指向未定义函数）。

检查项
  A 内置技能   src/python/skill_handlers.py 的 register_skill(name, func)
               → func 是否在本模块定义/导入；重名；风险级别合法性
  B 插件技能   src/python/plugin/*.py 的 @Skill(name=..) / @Command(name=..)
               → 被装饰函数是否存在；跨插件重名
  C 命令面     src/python/ai_server.py 的 if cmd == "X" / if cmd in (...)
               → 分发的 cmd_* 函数是否已定义；命令重名
  D 扩展命令表 src/python/ext_dispatch.py 的 EXT_MAP → (模块, 函数) 是否真实存在
  E 本地 import src/python/*.py 的裸模块导入 → 本地文件 / 标准库 / 已知三方；
               否则判为幽灵模块（ERROR）
  F C 统一 API src/registry/function_registry_builtin.c 的 api_register_function 清单（信息项）
  G 假数据标记 random 使用 / TODO / 占位（信息项，只报告不判死）

退出码：0 = 无 ERROR；1 = 存在 ERROR（可复跑，供 CI/人工使用）
用法：
  python3 tools/check_python_consistency.py                     # 报告 → docs/python_consistency_report.md
  python3 tools/check_python_consistency.py --json              # 仅输出 JSON
  python3 tools/check_python_consistency.py --repo /path/to/LINGOS-server
"""

import os
import re
import sys
import ast
import json
import argparse

# 已知可选第三方依赖（打包/安装时随包提供；不在此表的缺失模块 = 幽灵引用）
KNOWN_THIRD_PARTY = {
    "requests", "flask", "flask_cors", "flask_socketio", "PIL", "paddleocr",
    "sentence_transformers", "numpy", "cv2", "serial", "yaml", "websockets",
    "eventlet", "gevent", "tiktoken", "bs4", "lxml", "psutil", "pyaudio",
    "sounddevice", "edge_tts", "openai", "anthropic",
    "websocket", "pytesseract", "ultralytics", "onnxruntime",
}
VALID_RISKS = {"low", "medium", "high", "critical"}

# 本轮（task-10）已修复项——写入报告，便于复跑时对照历史
FIXED_ITEMS = [
    "① 预警级别桶补齐 info/L0-L5：原 ai_server.cmd_alert_summary 只有 L0~L3+info，"
    "C 端 L4/L5（生命线）被统计成 info → App 显示成提示级。新增 src/python/alert_store.py "
    "统一级别归一化/统计（含数值型与 level_num），并新增 highest_level。",
    "① alert_query 技能幽灵引用修复：原 from alert_manager import ... （alert_manager 只有 C 实现、"
    "无 Python 绑定）→ 技能恒失败。改读 /LINGOS/data/alerts（与命令面 cmd_alert_query 同源）。",
    "② 删除恒 True 且全仓库零读取点的死标志 _model_router_available / _vector_search_available"
    "（ai_server.py）。",
    "② 对话摘要失败时不再编造“已摘要 N 条”的假摘要（原文丢失）：改为保留原消息硬截断"
    "（信息保真，仍省 token）。",
]
MARKER_RE = re.compile(r"\brandom\s*\.|(?<![A-Za-z_])TODO\b|FIXME|占位|placeholder")


# =============================================================
# 工具函数
# =============================================================

def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def _tree(path):
    return ast.parse(_read(path), filename=path)


def _call_name(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return node.attr
    return ""


def _const_str(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _collect_strs(node):
    out = []
    if node is None:
        return out
    s = _const_str(node)
    if s is not None:
        return [s]
    for attr in ("elts", "values"):
        seq = getattr(node, attr, None)
        if isinstance(seq, (list, tuple)):
            for e in seq:
                out.extend(_collect_strs(e))
    return out


def _defined_symbols(tree):
    """模块内"可用符号"：函数/类 + 任意层 import 绑定名"""
    syms = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            syms.add(node.name)
        elif isinstance(node, ast.Import):
            for a in node.names:
                syms.add((a.asname or a.name).split(".")[0])
        elif isinstance(node, ast.ImportFrom):
            for a in node.names:
                syms.add(a.asname or a.name)
    return syms


def _add(store, level, check, msg, file="", line=0):
    store.append({"level": level, "check": check, "msg": msg,
                  "file": file, "line": line})


# =============================================================
# A. 内置技能（register_skill）
# =============================================================

def scan_registered_skills(tree):
    rows = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and _call_name(node.func) == "register_skill":
            name = _const_str(node.args[0]) if node.args else None
            func = None
            if len(node.args) > 1 and isinstance(node.args[1], ast.Name):
                func = node.args[1].id
            risk = "low"
            for kw in node.keywords:
                if kw.arg == "risk":
                    risk = _const_str(kw.value) or "?"
            rows.append({"name": name, "func": func, "risk": risk, "line": node.lineno})
    return rows


def check_skills(repo, out):
    path = os.path.join(repo, "src", "python", "skill_handlers.py")
    rel = "src/python/skill_handlers.py"
    if not os.path.exists(path):
        _add(out, "ERROR", "A", "skill_handlers.py 不存在", rel)
        return []
    tree = _tree(path)
    syms = _defined_symbols(tree)
    rows = scan_registered_skills(tree)
    seen = {}
    for r in rows:
        if not r["name"]:
            _add(out, "ERROR", "A", "register_skill 首参不是字符串常量（无法静态校验）", rel, r["line"])
            continue
        if r["func"] is None:
            _add(out, "WARN", "A", "register_skill(%s) 处理器非简单函数名，跳过" % r["name"], rel, r["line"])
        elif r["func"] not in syms:
            _add(out, "ERROR", "A", "技能 %s 指向未定义的处理器 %s()" % (r["name"], r["func"]), rel, r["line"])
        if r["risk"] not in VALID_RISKS:
            _add(out, "WARN", "A", "技能 %s 风险级别异常：%s" % (r["name"], r["risk"]), rel, r["line"])
        if r["name"] in seen:
            _add(out, "ERROR", "A", "技能重名 %s（先注册于第 %d 行）" % (r["name"], seen[r["name"]]), rel, r["line"])
        else:
            seen[r["name"]] = r["line"]
    return sorted(seen)


# =============================================================
# B. 插件技能（@Skill / @Command）
# =============================================================

def check_plugins(repo, out):
    pdir = os.path.join(repo, "src", "python", "plugin")
    rel_dir = "src/python/plugin"
    names = {}
    if not os.path.isdir(pdir):
        _add(out, "WARN", "B", "plugin 目录不存在", rel_dir)
        return []
    for fn in sorted(os.listdir(pdir)):
        if not fn.endswith(".py") or fn.startswith("_"):
            continue
        path = os.path.join(pdir, fn)
        try:
            tree = _tree(path)
        except SyntaxError as e:
            _add(out, "ERROR", "B", "插件语法错误：%s" % e, "%s/%s" % (rel_dir, fn), getattr(e, "lineno", 0))
            continue
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for dec in node.decorator_list:
                if not isinstance(dec, ast.Call):
                    continue
                dname = _call_name(dec.func)
                if dname not in ("Skill", "Command"):
                    continue
                sname = None
                for kw in dec.keywords:
                    if kw.arg == "name":
                        sname = _const_str(kw.value)
                if not sname and dec.args:
                    sname = _const_str(dec.args[0])
                if not sname:
                    _add(out, "WARN", "B", "@%s 未声明 name（函数 %s）" % (dname, node.name),
                         "%s/%s" % (rel_dir, fn), dec.lineno)
                    continue
                key = (dname, sname)
                if key in names:
                    _add(out, "WARN", "B", "%s 重名 %s（另一处 %s:%d）" % (dname, sname, names[key][0], names[key][1]),
                         "%s/%s" % (rel_dir, fn), dec.lineno)
                else:
                    names[key] = ("%s/%s" % (rel_dir, fn), dec.lineno)
    return sorted({n for _, n in names})


# =============================================================
# C. 命令面（ai_server if cmd == ...）
# =============================================================

def check_commands(repo, out):
    path = os.path.join(repo, "src", "python", "ai_server.py")
    rel = "src/python/ai_server.py"
    if not os.path.exists(path):
        _add(out, "ERROR", "C", "ai_server.py 不存在", rel)
        return []
    tree = _tree(path)
    syms = _defined_symbols(tree)
    mod_cache = {}
    cmds = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.If):
            continue
        test = node.test
        if not (isinstance(test, ast.Compare) and isinstance(test.left, ast.Name) and test.left.id == "cmd"):
            continue
        cnames = []
        for op, comp in zip(test.ops, test.comparators):
            if isinstance(op, (ast.Eq, ast.In)):
                cnames.extend(_collect_strs(comp))
        if not cnames:
            continue
        calls = set()
        attr_calls = set()
        for sub in ast.walk(ast.Module(body=node.body, type_ignores=[])):
            if not isinstance(sub, ast.Call):
                continue
            fn = sub.func
            if isinstance(fn, ast.Name):
                if fn.id.startswith("cmd_"):
                    calls.add(fn.id)
            elif isinstance(fn, ast.Attribute) and isinstance(fn.value, ast.Name):
                if fn.attr.startswith("cmd_"):
                    attr_calls.add((fn.value.id, fn.attr))
        for cn in sorted(calls):
            if cn not in syms:
                _add(out, "ERROR", "C", "命令 %s 分发的 %s() 未定义" % (cnames, cn), rel, node.lineno)
        # 跨模块处理器（module.cmd_xxx）→ 必须在该本地模块中真实定义
        for base, attr in sorted(attr_calls):
            mpath = os.path.join(repo, "src", "python", "%s.py" % base)
            if not os.path.exists(mpath):
                continue  # 非本地模块（对象方法等）不在本检查范围
            if base not in mod_cache:
                mod_cache[base] = _defined_symbols(_tree(mpath))
            if attr not in mod_cache[base]:
                _add(out, "ERROR", "C", "命令 %s 分发的 %s.%s() 在模块中不存在" % (cnames, base, attr),
                     rel, node.lineno)
        for c in cnames:
            if c in cmds:
                _add(out, "WARN", "C", "命令重名 %s（先前第 %d 行）" % (c, cmds[c]), rel, node.lineno)
            else:
                cmds[c] = node.lineno
    return sorted(cmds)


# =============================================================
# D. 扩展命令表 EXT_MAP
# =============================================================

def scan_ext_map(tree):
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "EXT_MAP" for t in node.targets):
            if isinstance(node.value, ast.Dict):
                rows = []
                for k, v in zip(node.value.keys, node.value.values):
                    key = _const_str(k)
                    if key and isinstance(v, ast.Tuple) and len(v.elts) == 2:
                        mod, fn = _const_str(v.elts[0]), _const_str(v.elts[1])
                        if mod and fn:
                            rows.append({"cmd": key, "module": mod, "func": fn, "line": v.lineno})
                return rows
    return []


def check_ext_map(repo, out):
    path = os.path.join(repo, "src", "python", "ext_dispatch.py")
    rel = "src/python/ext_dispatch.py"
    if not os.path.exists(path):
        _add(out, "ERROR", "D", "ext_dispatch.py 不存在", rel)
        return 0
    tree = _tree(path)
    rows = scan_ext_map(tree)
    if not rows:
        _add(out, "ERROR", "D", "EXT_MAP 未找到或不是静态字典", rel)
        return 0
    cache = {}
    for r in rows:
        mpath = os.path.join(repo, "src", "python", "%s.py" % r["module"])
        if not os.path.exists(mpath):
            _add(out, "ERROR", "D", "命令 %s 指向缺失模块 %s.py" % (r["cmd"], r["module"]), rel, r["line"])
            continue
        if r["module"] not in cache:
            cache[r["module"]] = _defined_symbols(_tree(mpath))
        if r["func"] not in cache[r["module"]]:
            _add(out, "ERROR", "D", "命令 %s 指向 %s.py 中不存在的 %s()" % (r["cmd"], r["module"], r["func"]),
                 rel, r["line"])
    return len(rows)


# =============================================================
# E. 本地 import 解析
# =============================================================

def scan_imports(tree):
    parents = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            parents[child] = parent
    rows = []
    for node in ast.walk(tree):
        roots = []
        if isinstance(node, ast.Import):
            roots = [a.name.split(".")[0] for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                continue
            roots = [(node.module or "").split(".")[0]]
        roots = [r for r in roots if r]
        if not roots:
            continue
        guarded = False
        nested = False
        p = parents.get(node)
        while p is not None:
            if isinstance(p, (ast.FunctionDef, ast.AsyncFunctionDef)):
                nested = True
            if isinstance(p, ast.Try):
                for h in p.handlers:
                    tnames = []
                    if isinstance(h.type, ast.Tuple):
                        tnames = [_call_name(e) for e in h.type.elts]
                    elif h.type is not None:
                        tnames = [_call_name(h.type)]
                    if any(x in ("ImportError", "ModuleNotFoundError", "Exception", "BaseException")
                           for x in tnames):
                        guarded = True
            p = parents.get(p)
        for r in sorted(set(roots)):
            rows.append({"module": r, "line": node.lineno, "guarded": guarded, "nested": nested})
    return rows


def check_imports(repo, out):
    pdir = os.path.join(repo, "src", "python")
    rel_dir = "src/python"
    stdlib = set(getattr(sys, "stdlib_module_names", frozenset()))
    local = set()
    for fn in os.listdir(pdir):
        if fn.endswith(".py"):
            local.add(fn[:-3])
        elif os.path.isdir(os.path.join(pdir, fn)) and os.path.exists(os.path.join(pdir, fn, "__init__.py")):
            local.add(fn)
    plugin_local = set()
    plug = os.path.join(pdir, "plugin")
    if os.path.isdir(plug):
        for fn in os.listdir(plug):
            if fn.endswith(".py") and not fn.startswith("_"):
                plugin_local.add(fn[:-3])
    checked = 0
    for fn in sorted(os.listdir(pdir)):
        if not fn.endswith(".py"):
            continue
        path = os.path.join(pdir, fn)
        try:
            tree = _tree(path)
        except SyntaxError as e:
            _add(out, "ERROR", "E", "语法错误：%s" % e, "%s/%s" % (rel_dir, fn), getattr(e, "lineno", 0))
            continue
        for imp in scan_imports(tree):
            checked += 1
            m = imp["module"]
            if m in stdlib or m in local or m in plugin_local:
                continue
            if m in KNOWN_THIRD_PARTY:
                if not imp["guarded"]:
                    # 模块级未保护 = 整模块可能起不来（WARN）；函数内 import = 仅该功能降级（INFO）
                    lvl = "INFO" if imp.get("nested") else "WARN"
                    where = "（函数内 import，仅该功能受影响）" if imp.get("nested") else "（模块级 import）"
                    _add(out, lvl, "E", "第三方依赖 %s 未用 try/except 保护%s" % (m, where),
                         "%s/%s" % (rel_dir, fn), imp["line"])
                continue
            _add(out, "ERROR", "E",
                 "幽灵模块 %s：既非本地文件，也不在已知依赖表（%s）" % (m, "guarded" if imp["guarded"] else "unguarded"),
                 "%s/%s" % (rel_dir, fn), imp["line"])
    return checked


# =============================================================
# F. C 统一 API 清单（信息项）
# =============================================================

def check_c_api(repo, out):
    path = os.path.join(repo, "src", "registry", "function_registry_builtin.c")
    rel = "src/registry/function_registry_builtin.c"
    if not os.path.exists(path):
        _add(out, "INFO", "F", "function_registry_builtin.c 不存在（跳过）", rel)
        return []
    cmds = re.findall(r'api_register_function\(\s*"([^"]+)"', _read(path))
    _add(out, "INFO", "F", "C 统一 API 命令 %d 条：%s" % (len(cmds), ", ".join(cmds)), rel)
    return cmds


# =============================================================
# G. 假数据标记（信息项）
# =============================================================

def check_markers(repo, out):
    pdir = os.path.join(repo, "src", "python")
    hits = 0
    for root, _dirs, files in os.walk(pdir):
        if "__pycache__" in root:
            continue
        for fn in sorted(files):
            if not fn.endswith(".py"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, repo)
            try:
                lines = _read(path).splitlines()
            except Exception:
                continue
            for i, line in enumerate(lines, 1):
                if not MARKER_RE.search(line):
                    continue
                hits += 1
                _add(out, "INFO", "G", "标记：%s" % line.strip()[:120], rel, i)
    return hits


# =============================================================
# 主流程 + 报告
# =============================================================

def run(repo):
    out = []
    skills = check_skills(repo, out)
    plugins = check_plugins(repo, out)
    cmds = check_commands(repo, out)
    ext_n = check_ext_map(repo, out)
    imports_n = check_imports(repo, out)
    c_api = check_c_api(repo, out)
    markers = check_markers(repo, out)
    summary = {
        "builtin_skills": len(skills),
        "plugin_skills": len(plugins),
        "commands": len(cmds),
        "ext_map_entries": ext_n,
        "imports_checked": imports_n,
        "c_api_commands": len(c_api),
        "markers": markers,
        "errors": sum(1 for f in out if f["level"] == "ERROR"),
        "warnings": sum(1 for f in out if f["level"] == "WARN"),
        "infos": sum(1 for f in out if f["level"] == "INFO"),
    }
    order = {"ERROR": 0, "WARN": 1, "INFO": 2}
    out.sort(key=lambda f: (order.get(f["level"], 9), f["check"], f["file"], f["line"]))
    return summary, out


def render_markdown(summary, findings):
    L = []
    L.append("# Python 侧一致性校验报告（清单 ↔ 实际 handler）")
    L.append("")
    L.append("生成方式（可复跑）：python3 tools/check_python_consistency.py")
    L.append("")
    L.append("## 统计")
    L.append("")
    L.append("| 项 | 数量 |")
    L.append("|---|---|")
    L.append("| 内置技能（register_skill） | %d |" % summary["builtin_skills"])
    L.append("| 插件技能（@Skill/@Command） | %d |" % summary["plugin_skills"])
    L.append("| 命令面（ai_server if cmd ==） | %d |" % summary["commands"])
    L.append("| 扩展命令表（EXT_MAP） | %d |" % summary["ext_map_entries"])
    L.append("| 已解析 import | %d |" % summary["imports_checked"])
    L.append("| C 统一 API 命令 | %d |" % summary["c_api_commands"])
    L.append("| 假数据/占位标记（信息项） | %d |" % summary["markers"])
    L.append("| **ERROR** | **%d** |" % summary["errors"])
    L.append("| WARN | %d |" % summary["warnings"])
    L.append("| INFO | %d |" % summary["infos"])
    L.append("")
    L.append("## 发现")
    L.append("")
    if not findings:
        L.append("（无）")
    else:
        L.append("| 级别 | 检查项 | 位置 | 说明 |")
        L.append("|---|---|---|---|")
        for f in findings:
            loc = "%s:%d" % (f["file"], f["line"]) if f["file"] else "-"
            L.append("| %s | %s | %s | %s |" % (f["level"], f["check"], loc,
                                                str(f["msg"]).replace("|", "\\|")))
    L.append("")
    L.append("## 本轮修复（task-10）")
    L.append("")
    for it in FIXED_ITEMS:
        L.append("- %s" % it)
    L.append("")
    L.append("## 检查项说明")
    L.append("")
    L.append("- A 内置技能：register_skill(name, func) 的 func 必须在本模块定义/导入；重名/风险级别")
    L.append("- B 插件技能：plugin/*.py 的 @Skill/@Command 装饰器 name 与重名")
    L.append("- C 命令面：ai_server.py 的 if cmd == 分支必须调用已定义的 cmd_* 函数")
    L.append("- D 扩展命令表：ext_dispatch.EXT_MAP 的 (模块, 函数) 必须真实存在")
    L.append("- E 本地 import：裸模块导入必须命中本地文件 / 标准库 / 已知依赖表，否则为幽灵模块")
    L.append("- F C 统一 API：function_registry_builtin.c 注册清单（信息项，供跨语言比对）")
    L.append("- G 假数据标记：random. / TODO / 占位（信息项，人工判定，不自动删除）")
    L.append("")
    return "\n".join(L) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="LINGOS Python 侧一致性校验（静态，可复跑）")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="仓库根目录（默认脚本上级目录）")
    ap.add_argument("--out", default="docs/python_consistency_report.md",
                    help="Markdown 报告输出路径（相对仓库根）")
    ap.add_argument("--json", action="store_true", help="只输出 JSON 到 stdout，不写报告")
    args = ap.parse_args(argv)

    repo = os.path.abspath(args.repo)
    if not os.path.isdir(os.path.join(repo, "src", "python")):
        print("ERROR: %s 不是 LINGOS-server 仓库根" % repo, file=sys.stderr)
        return 2

    summary, findings = run(repo)

    if args.json:
        print(json.dumps({"summary": summary, "findings": findings}, ensure_ascii=False, indent=2))
        return 1 if summary["errors"] else 0

    out_path = args.out if os.path.isabs(args.out) else os.path.join(repo, args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(render_markdown(summary, findings))

    print("内置技能 %d / 插件技能 %d / 命令面 %d / EXT_MAP %d / import %d" % (
        summary["builtin_skills"], summary["plugin_skills"], summary["commands"],
        summary["ext_map_entries"], summary["imports_checked"]))
    print("ERROR %d / WARN %d / INFO %d -> %s" % (
        summary["errors"], summary["warnings"], summary["infos"], out_path))
    for f in findings:
        if f["level"] == "ERROR":
            print("  [ERROR] %s %s:%s %s" % (f["check"], f["file"], f["line"], f["msg"]))
    return 1 if summary["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
