#!/usr/bin/env python3
"""LING OS 插件模板（0.4.4）

用法：
  1. 复制本文件到 /LINGOS/plugins/my_plugin.py（或任意 .py）
  2. 修改 name / 加入 @Skill 与 @Command
  3. App/Web 执行 `plugin_reload`，或重启生效
  4. 验证：`plugin_list` 应列出本插件及其技能/命令

约定：
  · 类必须继承 Plugin 且模块内**只定义一个**插件类（多类时取首个）
  · @Skill  -> 可被 AI 作为工具调用（进 skill_schemas）
  · @Command -> 可被 shell/App 以命令名调用
  · 技能函数签名固定为 func(args_json: str) -> (bool, str)
"""
import json
import logging

from lingos_plugin import Plugin, Skill, Command

logger = logging.getLogger("MyPlugin")


class MyPlugin(Plugin):
    """示例插件——展示技能与命令的定义方式"""

    name = "my_plugin"          # 唯一标识（勿与内置技能重名）
    version = "0.1.0"
    description = "插件模板（复制后改造）"
    author = "LING OS"
    type = "utility"            # 插件类型（自定义）

    def on_load(self) -> bool:
        """加载时调用（返回 False 表示加载失败）"""
        logger.info("my_plugin loaded")
        # 可在此读取配置：/LINGOS/system/config/xxx.json
        self._greeting = "你好"
        return True

    def on_unload(self) -> bool:
        """卸载时调用（释放资源）"""
        logger.info("my_plugin unloaded")
        return True

    # ---------------- 技能（AI 可调用） ----------------
    @Skill(name="my_plugin_hello",
           risk="low",
           description="示例技能：回显问候语。参数 who 为称呼。")
    def hello(self, args_json: str):
        try:
            args = json.loads(args_json) if args_json else {}
        except Exception:
            args = {}
        who = args.get("who", "世界")
        return True, f"{self._greeting}，{who}！（来自 my_plugin）"

    # ---------------- 命令（shell/App 可调用） ----------------
    @Command(name="myplugin_status", description="示例命令：返回插件状态")
    def status(self, args: str = ""):
        return f"my_plugin v{self.version} 运行中"
