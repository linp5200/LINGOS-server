# LING OS 服务端更新日志（CHANGELOG）

版本线：发行版 0.4.3（server/app 统一——先生 2026-09-04 规范）· 内部 LN-0.4.3
包名（新规范）：LINGOS_server_linux_v<版本>_<架构>_<allbin|sysbin|plugin>.tar.gz

---

## [0.4.3] - 2026-09-04（系统重构部署批次——先生全权授权）

### 新增（Features）
- **Web UI 网页访问（先生重点要求）**：
  - `POST /api/cmd` 命令代理（http_server → ai.sock → JSON）——网页/任意 HTTP 客户端统一命令入口
  - `GET /ui` `/console` `/` 静态 UI 页：从 `<root>/share/webui/index.html` 读（可热更新；缺文件回退内嵌页）
  - 完整功能 Web UI 首版（webui/index.html——17 页：控制台/AI对话/会话/提供商/技能/记忆/人格/预警/天气/视觉/HA/文件/日志/端口/更新/系统/设置，FUI v2 地形风格）
  - 真数据绑定：/system/health + /api/cmd 拉取会话/预警/健康实时填充（失败显示 -- 不模拟）
- **预警实时广播**：alert_notify.c 增加 libcurl POST `/api/alert_event`（仿 visiond）→ ai_server WS 广播 `alert_event`——App/Qt/Web 实时弹条（原仅轮询）
- **天气系统（Open-Meteo）**：
  - `cmd_weather_current`——实时天气（温度/体感/湿度/风/气压/能见度/UV）+ JSON 缓存 10min
  - `cmd_weather_forecast`——逐小时 24h + 7 日预报 + 缓存 6h
  - 天气源可切换：open-meteo（默认）/ wttr.in 兜底 / 用户自定义 API（custom_url——先生裁决）
  - 配置 `/LINGOS/system/config/weather.json`（城市 lat/lon/api/custom_url）
- **命名规范 v0.4.3**（先生 09-04 定稿）：`LINGOS_<server|app>_<linux|win|android>_v<版本>_<架构>_<plugin|allbin|sysbin>`
  - bundle.sh：allbin（原全捆）+ **sysbin**（仅二进制+py，README 注明依赖）双包产出
- **内部版本统一**：`LN-B-5.1.2.6-rc` → **`LN-0.4.3`**（先生：去 B 跟发行版；100 文件全量替换）

### 基础设施（0.4.3 路径集中化起步）
- `lingos_data_root()` 支持 `LINGOS_ROOT` 环境变量覆盖（全捆 start.sh 已导出——包内 share/webui 随包生效，部署零拷贝）
- env_bootstrap 目录清单增加 `/share` `/share/webui`

---

## [0.0.4] - 2026-08-23（配置优化批次——先生裁决定稿实施）

### 修复（Bug Fixes）
- **健康自检"配置完成但被误判未完成"根因修复**（先生重点指出）：
  - 根因：`configured_at` 字段仅初始化=0，**从未置位**→ 自检 `configured_at==0` 永远判定"配置不完整，需要运行配置向导"→ 用户被迫重新配置
  - 修复：`config_core_mark_configured` 写 state.json 同时**同步内存 configured_at**；`config_core_load` 从 state.json 恢复 `system_configured`→configured_at（重启后自检也通过）
- **配置向导回车逻辑**（先生裁决）：有默认值→回车=采用默认；**无默认值→回车提示"尚未选择"**，不进入下一阶段（原为"空输入→降级切换模式"）
  - wizard_engine.c SELECT/INPUT 步骤均适配；补 uart.h include

### 新增（Features）
- **日志体系重构（先生定稿——服务端/客户端拆分）**：
  - `/Debug` 目录删除 → 统一 `/log` 单文件（C 端 log_extra.c + Python ai_server.py）
  - 文件存储 JSON 四字段：`time(ISO8601毫秒带时区)/id(自增)/level/txt(原终端内容含[LEVEL][模块][函数]标识，无颜色码)`
  - 终端显示：保留原格式仅时间改 `[时间]`；颜色=低级**冷灰**/WARN**黄**/ERROR**鲜红**（原 INFO 绿/DEBUG 青）
  - 文件保存开关 `log_set_file_output`/`set_log_file_enabled`：默认开=DEBUG 全量；关=仅 WARN+
- **指令翻译层（动词子命令，先生定稿 git/docker 风格）**：
  - 形式二（动作 领域）：`list model` / `list session` / `view status` / `query balance` 等
  - 形式一变体（领域 动作）：`model switch <id>` / `session create|delete|history` / `skill enable|disable` / `memory search|write` / `ha status|states` / `voice usage` / `provider list`
  - WS 机器命令（session_list 等 JSON 字面量）**保留原样**；难适配指令（token 族/allow-high-risk/logdump 专名）**保留原样**
  - 实现：shell.c `handle_verb_command` + `send_ai_command`（daemon socket 转发，select 5s 超时防阻塞）
- **增量包 JSON manifest（先生定稿约定格式）**：
  - 新建 update_incremental_json.c/h：`{base_ver, target_ver, files:[{path,action:add/mod/del,size,hash}]}`
  - base_ver 匹配校验（不匹配拒绝 -2）+ 文件 sha256 校验 + 应用前备份（.inc_backup 可回滚）+ add/mod 先 del 后
- **技能安装（OpenClaw 式，先生定稿）**：
  - 新建 skill_install.py：技能包格式 `SKILL.md + handler.py + requirements.txt`（SKILL.md YAML front-matter：name/description/risk/handler）
  - **内置技能**（.builtin 标记）不可删除；**自定义技能**可增删
  - 命令：`skill_install`(src) / `skill_uninstall`(name) / `skill_list_custom` / 自动注册到 SKILL_REGISTRY（调用适配免手写）
- **三级记忆（先生定稿纳入本批）**：
  - L1 main（重要常驻，`[L1][重要]` 前缀自动注入）/ L2 working（`[L2]` 工作记忆）/ L3 external（默认，按需检索）
  - memory_write 支持 `level: l1/l2/l3`（importance=high 自动 L1）；memory_retrieval 支持 level 过滤 + find_working

### 已知（Known）
- 端口不可在配置中更改（先生裁决）——改端口用特定指令族（port 指令待后续批次）
- App 端日志：同格式（time/id/level/txt），默认不保存、导出落盘、显示最近 100 行——App 端实施在 App 仓库批次

---

## [0.0.3] - 2026-08-15（DeepSeek 扩展批次）
- #3 空回复/工具调用 400 根因修复（reasoning_content 回传/流式中断检测/空回复重试）
- DeepSeek 思考开关/强度（仅 DeepSeek 系）、balance_query、model_list_query
- provider 同步修复（provider_add 同步）

## [0.0.2] - 2026-08-14（0.2.2 批次）
- 同步协议（sync_full/delta/归属/冲突）、HA 部署引导、vision 六层、插件化预留

## [0.0.1] - 2026-08-14（0.2.1 批次）
- HA 集成（ha_integration.py + 5 命令 + ha_control 权限）、9.4 cmd 匹配、9.1 token 统计、自检优化、CI 双架构全捆包
