#!/usr/bin/env python3
"""服务端 9.4 改造：命令响应统一注入 cmd 标识（App 按 cmd 匹配，防与自动同步冲突）"""
import re, sys

SRC = "/var/minis/workspace/lingos-server/src/python/ai_server.py"
with open(SRC, "r", encoding="utf-8") as f:
    content = f.read()

# 1) 在 TOKEN_USAGE_FILE 定义前插入 _reply 帮助函数
anchor = 'TOKEN_USAGE_FILE = "/LINGOS/state/token_usage.jsonl"'
reply_fn = '''def _reply(conn, cmd_name, resp):
    """命令响应统一出口——注入 cmd 标识（9.4：App 按 cmd 匹配，防与自动同步响应冲突先到先得）"""
    try:
        if isinstance(resp, dict):
            resp = dict(resp)
            resp["cmd"] = cmd_name
        conn.send((json.dumps(resp, ensure_ascii=False) + "\\n").encode())
    except Exception as e:
        logger.debug("reply send failed cmd=%s: %s", cmd_name, e)

'''
assert anchor in content, "anchor not found"
content = content.replace(anchor, reply_fn + anchor, 1)

# 2) 批量替换 conn.send((json.dumps(cmd_xxx(...), ...) 模式
#    匹配：conn.send((json.dumps(cmd_xxx(args), ensure_ascii=False) + "\n").encode());
#    或：conn.send((json.dumps(cmd_xxx(args), ensure_ascii=False) + "\n").encode())
pattern = re.compile(
    r'conn\.send\(\(json\.dumps\((cmd_\w+)\((.*?)\), ensure_ascii=False\) \+ "\\n"\)\.encode\(\)\)',
    re.DOTALL
)

def repl(m):
    func = m.group(1)          # cmd_session_list
    args = m.group(2).strip()  # 参数
    cmd_name = func[4:]        # session_list
    return f'_reply(conn, "{cmd_name}", {func}({args}))'

content, n = pattern.subn(repl, content)
print(f"替换命令响应: {n} 处")

with open(SRC, "w", encoding="utf-8") as f:
    f.write(content)
print("写入完成")
