# LING OS Python 插件层（0.4.4 起真正启用）

## 目录
- 插件安装位置：`/LINGOS/plugins/*.py`（用户可增删）
- 本目录 = 插件**框架**（随包分发）：基类 / 发现 / 加载器 / 模板

## 启用状态
| 项 | 0.4.3 及以前 | 0.4.4 |
|---|---|---|
| `load_plugins()` 被调用 | ❌ 从未调用（死代码） | ✅ ai_server 启动时加载 |
| 插件技能进 AI 工具表 | ❌ | ✅ 并入 `skill_schemas` |
| 插件技能可执行 | ❌（`execute_skill` 只查内置表） | ✅ 内置表未命中 → 回退插件 |
| App/Web 可管理 | ❌ | ✅ `plugin_list` / `plugin_reload` |

## 写一个插件
1. 复制 `_plugin_template.py` → `/LINGOS/plugins/my_plugin.py`
2. 改 `name`，按需加 `@Skill`（AI 可调用）/ `@Command`（命令可调用）
3. App/Web 执行 `plugin_reload` 热重载
4. `plugin_list` 验证

## API
- `GET/POST /api/cmd` body: `{"cmd":"plugin_list"}` → 已加载插件/技能/命令
- `{"cmd":"plugin_reload"}` → 重新扫描 `/LINGOS/plugins/`
- 技能函数签名：`def f(args_json: str) -> (bool, str)`

## 现有扩展点（vision 链）
`video_source` / `detection` / `ocr_engine` / `calibration` / `presenter` / `vision_ai`
（见 `docs/PLUGIN_ARCHITECTURE.md`；示例：`rtsp_source_plugin.py`）
