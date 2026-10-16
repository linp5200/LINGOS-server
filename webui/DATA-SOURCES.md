# WebUI 数据来源对照（第6期 · 假数据清零后）

服务端仓库根：`/root/MIGRATION-20261007/repos/LINGOS-server`
本页所有数据来自真实命令；**无数据一律显示 `--`（不模拟）**。

## C 侧调用点（网页如何被服务 / 命令如何被转发）
| 作用 | 位置 |
|---|---|
| 静态页 `/ui` `/console` `/` 读取 `<root>/share/webui/index.html`（缺文件回退内嵌页） | `src/api/http_server.c:331-337`（内嵌回退页 `http_server.c:58`） |
| `POST /api/cmd` 命令代理（限流/鉴权 → `ai.sock`） | `src/api/http_server.c:518-563`，转发 `http_server.c:267-291` |
| `GET /system/health`（顶栏 LINKED/OFFLINE、设置页主机卡） | `src/api/http_server.c:659` |
| 打包装入 `share/webui/` | `scripts/bundle.sh:279-282` |
| `registry_list` 的真实实现（lingosd 命令） | `src/daemon/lingosd.c:634` |

> ⚠ 注意：`/api/cmd` 只代理到 **ai.sock（Python ai_server）**。`registry_list` 是 lingosd 命令，
> ai_server 未实现该命令（`src/python/ai_server.py:5830` 回「未知命令」），因此
> 「系统与插件 → 注册表」卡改为**诚实降级**：`skill_list_full` + `command_list` + `plugin_list` 实读汇总，
> 并显式标注「registry_list 经 /api/cmd 不可达」。原实现是永久空卡。

## 页面 → 命令 → 服务端处理器
| 页面 | 命令 | 服务端处理器（文件:行） |
|---|---|---|
| 控制台 顶栏/页脚版本 | `system_info` | `src/python/ai_server.py:3750` |
| 控制台 指标/TOKEN趋势/系统事件/预警卡 | `system_info` `token_usage_query` `notify_list` `alert_query` | `ai_server.py:3750` / `:5229` / `src/python/ux_ext.py:146` / `ai_server.py:4133` |
| AI 对话 | WS 2939（`auth`/`chat`/`interrupt`） | 协议 v3 |
| 会话管理 | `session_list` | `ai_server.py` 分发（`cmd == "session_list"`） |
| 模型与提供商 | `provider_list` | `ai_server.py:4897` |
| 技能管理 | `skill_list_full` | `ai_server.py:4421` |
| 预警 + 数据源健康 | `alert_query` `alert_summary` | `ai_server.py:4133` / `:4178` |
| 记忆 | `memory_search` | `ai_server.py:4221` |
| 人格 | `personality_get` `personality_set` | `ai_server.py:4784` / `:4772` |
| 天气 | `weather_current` `weather_forecast` `weather_code_map` | `ai_server.py:345` / `:406` / `:4206` |
| 视觉 | `monitor_status` `monitor_list` `ai_vision_status` | `src/python/monitor_service.py:98` / `:87`；`ai_server.py` |
| 智能家居（HA） | `ha_status` `ha_states`（控制 `ha_control`） | `src/python/ha_integration.py:129` / `:181` / `:144` |
| 智能家居（home_ext） | `home_overview` `scene_list` `area_list` `zone_list` `energy_summary` `automation_list` `discovery_list` | `src/python/ext_dispatch.py:55,33,25,43,37,49,23` |
| 监控时间线 | `nvr_overview` `timeline_segments` `timeline_query` `storage_cleanup` | `ext_dispatch.py:81,62,61,67` |
| 通知中心 | `notify_list` `notify_mark_read` `notify_clear` `notify_dnd` `notify_push` | `ux_ext.py:146,162,176,186,97` |
| 仪表盘 | `dashboard_get` `dashboard_add_card` `dashboard_reset` | `ext_dispatch.py:100`；`ux_ext.py` |
| 媒体 | `media_list` `media_command` | `ext_dispatch.py:106`；`ux_ext.py` |
| 知识库 | `kb_list` `kb_upload` `kb_ask` | `ext_dispatch.py:85`；`ai_ext.py` |
| 可选项 | `options_list` `options_set` `privacy_mode` | `ai_server.py:1542` / `:1584` |
| 命令面板 | `command_list` | `ai_server.py:4620` |
| 文件 | `file_list` `file_read`（真实目录树 + 逐级浏览） | `ai_server.py:3843` |
| 日志 | `file_read` `/LINGOS/log/lingos.log`（JSON 行） | `ai_server.py` |
| 端口与网络 | `port_list`（真实 connect 探测） | `ai_server.py:4656` |
| 升级更新 | `update_check` | `ai_server.py:4693` |
| 系统与插件 | `system_info` `plugin_list` `plugin_reload` `registry_list`(降级) | `ai_server.py:3750` / `:1646` / `:1651`；`lingosd.c:634` |
| 设置与高级 | `config_get` `system_info` `/system/health` | `ai_server.py:4879` / `:3750`；`http_server.c:659` |

## 静态自检（可复跑）
```bash
python3 - <<'PY'   # 1) JS 语法 + 2) id 接线 + 3) 导航↔页面
import re
src=open('webui/index.html',encoding='utf-8').read()
scripts=re.findall(r'<script>(.*?)</script>', src, re.S)
open('/tmp/w.js','w',encoding='utf-8').write('\n;\n'.join(scripts))
ids=set(re.findall(r'id="([A-Za-z0-9_\-]+)"', src))
js='\n'.join(scripts)
refs=set(re.findall(r"getElementById\('([A-Za-z0-9_\-]+)'\)", js))
print('缺失 id:', sorted(r for r in refs if r not in ids))
PY
node --check /tmp/w.js
```
