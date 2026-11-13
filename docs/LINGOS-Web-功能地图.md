# LING OS Web 功能地图（前端 ↔ 系统实现对照）

> 2026-11-14 · 给 Web 前端做功能入口用。全部条目都标了实现位置，能点的才放进界面。
> 命令面来自 `src/shell/dotted_cmd.c`（86 条 `系统.动作`）；Web 面来自 `services/lingos_web/server.py`；Agent 面来自 `src/python/`。

## 一、系统类目（唯一权威：`docs/ARCHITECTURE_系统类目.md`）

| 类目 | 系统 | 命令前缀 | 实现位置 | Web 入口 |
|---|---|---|---|---|
| 核心 | core | `core.*` | `src/core`、`src/daemon` | 系统页 · 核心状态 / 日志级别 / 注册表 |
| 通讯 | comm | `comm.*` | `src/api`、`src/net` | 系统页 · 端口与服务模式 |
| 通知 | notify | `notify.*` | `src/core`（通知分发） | 设置 · 通知（待接） |
| 语音 | voice | `voice.*` | `src/python/ai_server.py`、`src/voice` | 设置 · 语音用量（`voice.query`/`voice.usage`） |
| 配置 | config | `config.*` | `src/config` | 设置 · 高级配置（`config.get/list/set/advanced/reload`） |
| 数据 | data | `data.*` | `src/config`、`src/python/memory_*` | AI 页 · 记忆 / 会话 / 模型 |
| 插件 | plugin | `plugin.*` | `src/plugin`、`distro/plugins` | 系统页 · 插件列表 / 加载 / 卸载 |
| 安全 | security | `security.*` | `src/security`、`src/firewall` | 安全页 · 等级 / 审计 / 权限 / 行为 |
| 家居 | home | `home.*` | `src/python/home_ext.py`、`ha_integration.py` | 设备页 · HA / MQTT / 米家 |
| 监控 | monitor | `monitor.*` | `src/monitor`（lingos-monitor） | 摄像头 · 预览（默认关，未接不占位） |
| 预警 | alert | `alert.*` | `src/alert`（lingos-alertd） | 首页 · 预警历史 |
| AI | ai | `ai.*` | `src/ai`、`src/python/ai_server.py` | AI 页 · 对话 / 状态 / Provider |
| 技能 | skill | `skill.*` | `src/python/skill_*` | AI 页 · 技能库 |
| AI 识别 | ai_vision | `ai_vision.*` | `src/vision` | 设置 · 识别叠加开关（默认关） |
| OCR | ocr | `ocr.*` | `src/python/ocr_service.py` | 后续 |
| 天气 | weather | `weather.*` | `src`（weather 模块） | 首页小卡（后续） |
| 规则 | rules | `rules.*` | `src/rules` | 安全页 · 规则开关 |
| 更新 | update | `update.*` | `src/update` | 系统页 · 更新 / 回滚 |
| 快照 | snapshot | `snapshot.*` | `src/install` | 系统页 · 快照 / 回滚 |
| 修复 | repair | `repair.*` | `src/python/repair_engine.py` | 系统页 · 自检 / 修复 |

## 二、统一命令面（`/api/cmd`，代理到 127.0.0.1:18080）

前端统一调用：`POST /api/cmd {"cmd":"system.status","args":{}}`。18080 未启动时 Web 返回 502，前端显示「系统命令服务未启动」，不伪造结果。

现有 86 条（按前缀）：

- core：`status` `health` `control` `dashboard` `debug` `logdump` `log_level` `registry_list` `registry_show` `registry_check` `registry_categories` `registry_reload`
- system：`status` `health` `security` `defense` `privilege` `update` `rollback`
- config：`get` `set` `list` `reload` `advanced`
- data：`memory_list` `memory_search` `memory_write` `model_list` `session_list`
- memory：`list` `search` `write`
- security：`status` `audit` `perm_list` `behavior` `input` `privilege_status`
- home：`status` `states`
- alert：`status` `config` `history`
- ai：`status` `config` `provider_list` `repair` `nook_ask` `nook_status`
- skill：`list` `search` `browse` `enable` `disable` `install` `uninstall`
- plugin：`list` `load` `unload`
- rules：`list` `add` `toggle`
- reminder / remind：`list`
- registry：`list` `show` `check` `reload` `categories`
- health：`trend` `watchdog`
- comm：`port_list` `port_set` `server_mode`
- voice：`query` `usage`
- install：`app` `skill`
- snapshot：`create` `list`
- repair：`run`
- update：`install` `rollback`
- security / system 的等级与策略见安全页
- log：`level`；ui：`desktop`；control：`run`

## 三、Web API（`services/lingos_web/server.py`，cookie 会话 / Bearer）

| 能力 | 接口 | 说明 |
|---|---|---|
| 健康/版本 | `GET /api/health` | 公开；`setup` 判断是否首次 |
| 当前账号 | `GET /api/me` | 401 未登录 |
| 应用清单 | `GET /api/apps` | 6 个内置 app |
| 设备 | `GET/POST /api/devices` | 列表 / 加虚拟设备 |
| 设备命令 | `POST /api/devices/{id}/command` | `on/off/toggle/brightness/lock/unlock` |
| 设备发现 | `POST /api/devices/discover` | 走各适配器 |
| 适配器 | `GET /api/adapters` | virtual/ha/mqtt/miio 可用性 |
| 事件 | `GET /api/events` | 最近 100 条 |
| 安全 | `GET /api/security`、`POST /api/security/level` | L0–L4、平台、审计 |
| 设置 | `GET/POST /api/settings` | ha_url/ha_token/mqtt_*（admin） |
| 用户 | `GET/POST /api/users`、`DELETE /api/users/{id}` | admin |
| ACL | `GET/POST /api/acl` | 写要 root key |
| 文件 | `GET /api/fs/list`、`/api/fs/read`、`POST /api/fs/write`、`DELETE /api/fs?path=` | ACL + 受保护路径 |
| 审计导出 | `GET /api/audit/export` | JSON |
| 备份/恢复 | `GET /api/backup`、`POST /api/restore` | tar.gz，恢复要 root key |
| AI 对话 | `POST /api/chat` | 走 `run/ai.sock`，失败如实返回 |
| 命令代理 | `POST /api/cmd` | 到 18080 |
| 登录/初始化 | `POST /api/login`、`/api/setup`、`/api/logout`、`/api/pair` | 局域网首次要配对码 |
| App Token | `POST /api/token` | admin，只显示一次 |
| 调试台 | `/debug.html` | 只读诊断 |

## 四、设备接入（`services/lingos_device/manager.py`）

| 协议 | 实现 | 状态 | 需要 |
|---|---|---|---|
| virtual | 内置 | 总是可用 | 无 |
| HA | REST + Long-Lived Token | 配了才可用 | `ha_url` + `ha_token` |
| MQTT | paho-mqtt 可选 | 配了才可用 | `mqtt_host` / `mqtt_port` |
| miio | python-miio | 实验 | 设备 token |

命令语义：灯 `on/off/toggle/brightness{value}`；锁 `lock/unlock`；传感器 `state` 合并；开关 `last_cmd`；摄像头当前只读（`state.stream` 有值才预览）。

## 五、Agent 层（`src/python/`）

- 对话与路由：`ai_server.py`、`llm_unified.py`（多 Provider）、`agent_runtime.py`、`agent_orchestrator.py`
- 记忆与会话：`memory_pipeline.py`、`memory_retrieval.py`、`data.memory_*`
- 技能与扩展：`skill_loader.py`、`skill_install.py`、`skill_dev.py`、`ext_dispatch.py`、`git_skills.py`
- 家居：`home_ext.py`、`ha_integration.py`、`ha_archive.py`
- 监控/视觉：`monitor_service.py`、`rtsp_streamer.py`、`ocr_service.py`、`calibration_service.py`
- 安全与危机：`permission_gateway.py`、`authorization_service.py`、`crisis.py`、`crisis_delivery.py`
- 内容接入：`ingest_manager.py` + `ingest_plugins/`
- 远程：`remote_access.py`（VNC/远程状态）
- MCP：`mcp_client.py`
- 自检修复：`diagnosis_engine.py`、`repair_engine.py`

## 六、前端入口设计（Web = 图形化启动器）

| 页面 | 现在 | 下一步 |
|---|---|---|
| 家 | 设备聚合、事件、安全等级、可选摄像头 | 天气小卡、预警历史 |
| 设备 | 列表/命令/发现/适配器 | 场景与自动化（rules/home）、米家设备 |
| 文件 | 列表/读/写/上传/删除 | 目录 ACL 编辑 |
| 安全 | L0–L4、审计、用户 | 权限清单、行为、输入保护、App Token、ACL、规则 |
| AI | 对话（/api/chat） | 技能库、模型 Provider、记忆、会话 |
| 设置 | 后台地址、外观、摄像头预览、HA/MQTT、备份、root key | 语音、通知、远程/VNC、插件、高级配置、系统命令浏览器 |
| 系统（新增） | — | 核心状态、健康、更新/回滚、快照、修复、日志级别、命令浏览器（/api/cmd） |

## 七、原则

1. 没有后端 → `err-noserver`，不渲染任何模拟数据。
2. 命令服务未连 → 明确显示「系统命令服务未启动」，不伪造状态。
3. 摄像头预览默认关，没画面就不占位。
4. 危险操作（开锁、切安全等级、删用户、恢复备份）必须二次确认。
