#!/usr/bin/env python3
"""【R9】技能开发工具 - 创建/测试技能（CLI 版）
用法: skill_dev.py create <name> [desc] | test <name> [args_json] | list
"""
import json, os, sys, importlib.util

SKILL_DIR = "/LINGOS/skills/enabled"


def create(name, desc=""):
    if not name or not name.isidentifier():
        print("错误: 技能名 '{0}' 非法（需为合法标识符）".format(name))
        return 1
    skill_path = os.path.join(SKILL_DIR, name)
    if os.path.exists(skill_path):
        print("错误: 技能 '{0}' 已存在".format(name))
        return 1
    os.makedirs(skill_path, exist_ok=True)
    manifest = {
        "name": name,
        "description": desc or (name + " 技能"),
        "version": "1.0.0",
        "risk": "low",
        "handler": "python",
        "handler_path": name,
        "parameters": {"type": "object", "properties": {}, "required": []}
    }
    with open(os.path.join(skill_path, "skill.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    handler_tpl = (
        "def handle(args_json: str):\n"
        '    """{desc} - 技能实现"""\n'
        "    import json\n"
        "    try:\n"
        "        args = json.loads(args_json) if args_json else {}\n"
        "    except Exception:\n"
        "        args = {}\n"
        "    # TODO: 实现技能逻辑\n"
        '    result = {"ok": True, "message": "{name} 执行成功", "args": args}\n'
        "    return True, json.dumps(result, ensure_ascii=False)\n"
    )
    handler = handler_tpl.replace("{desc}", desc or name).replace("{name}", name)
    with open(os.path.join(skill_path, name + ".py"), "w", encoding="utf-8") as f:
        f.write(handler)
    print("✅ 已创建技能 '{0}': {1}".format(name, skill_path))
    print("   实现文件: {0}.py（编辑 handle 函数）".format(name))
    print("   测试: skill test {0} '{{}}'".format(name))
    return 0


def test(name, args_json="{}"):
    path = os.path.join(SKILL_DIR, name)
    if not os.path.isdir(path):
        path = os.path.join("/LINGOS/skills/market", name)
    if not os.path.isdir(path):
        print("错误: 技能 '{0}' 不存在".format(name))
        return 1
    py_file = os.path.join(path, name + ".py")
    if not os.path.exists(py_file):
        py_file = os.path.join(path, "handler.py")
    if not os.path.exists(py_file):
        print("错误: 未找到实现文件（{0}.py / handler.py）".format(name))
        return 1
    spec = importlib.util.spec_from_file_location(name, py_file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not hasattr(mod, "handle"):
        print("错误: 实现文件缺少 handle(args_json) 函数")
        return 1
    print("▶ 测试技能: {0}".format(name))
    print("  参数: {0}".format(args_json))
    try:
        ok, result = mod.handle(args_json)
        print("  " + ("✅ 成功" if ok else "❌ 失败") + ": " + str(result)[:500])
        return 0 if ok else 2
    except Exception as e:
        print("  ❌ 异常: {0}".format(e))
        return 2


def skill_list():
    if not os.path.isdir(SKILL_DIR):
        print("（无已安装技能）")
        return 0
    for d in sorted(os.listdir(SKILL_DIR)):
        if os.path.isdir(os.path.join(SKILL_DIR, d)):
            print("  - " + d)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: skill_dev.py create <name> [desc] | test <name> [args_json] | list")
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "create" and len(sys.argv) >= 3:
        sys.exit(create(sys.argv[2], " ".join(sys.argv[3:])))
    elif cmd == "test" and len(sys.argv) >= 3:
        sys.exit(test(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "{}"))
    elif cmd == "list":
        sys.exit(skill_list())
    else:
        print("用法: skill_dev.py create <name> [desc] | test <name> [args_json] | list")
        sys.exit(1)
