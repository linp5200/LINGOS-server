# LING OS 服务端更新日志（CHANGELOG）

版本线：发行版 0.4.3（server/app 统一——先生 2026-09-04 规范）· 内部 LN-0.4.3
包名（新规范）：LINGOS_server_linux_v<版本>_<架构>_<allbin|sysbin|plugin>.tar.gz

---

## [0.5.0] - 2026-09-12（安全底座 + 链接适配 22.04~25.10）

### 🔗 链接适配（先生实测：22.04 编译的二进制在 25.10 无法启动）
**根因**：11 个版本敏感 soname，来自**三条独立依赖链**
| 问题库 | 来源 | 处理 |
|---|---|---|
| libldap-2.5 / liblber-2.5 | libcurl（LDAP） | 去 curl |
| libunistring.2（链一） | libcurl→libpsl→libidn2 | 去 curl |
| libavcodec.58 / libavformat.58 / libswscale.5 / libavutil.56 | **libnotcurses**（TUI→ffmpeg） | 关 TUI |
| libunistring.2（链二） | **libmicrohttpd**→libgnutls30→libidn2-0 | 内置 HTTP 服务器 |

**成果**：`lingos_linux` 依赖 **100+ → 6 个**
```
libseccomp.so.2  libsqlite3.so.0  libmosquitto.so.1
libssl.so.3      libcrypto.so.3   libcares.so.2
```
（6 个库 soname 在 22.04~25.10 间完全稳定，已用 packages.ubuntu.com 逐项核对）

### 新增（Features）
- **`src/net/http_client.{h,c}`**：轻量 HTTP 客户端
  - 纯 socket 实现（零依赖，内网上报用）
  - libcurl 改为 **dlopen 可选**（`libcurl.so.4` / `libcurl-gnutls.so.4`）
- **`src/net/mhd_compat.{h,c}`**：**内置 HTTP 服务器**（libmicrohttpd 兼容层）
  - raw socket + pthread，覆盖 http_server.c 全部 116 处调用
  - 语义对齐 MHD 三段式上传回调
- **`src/security/safe_exec.{h,c}`**：安全命令执行（OWASP 三层）
  - 不经 shell（fork+execvp+argv）、参数分离、命令白名单 60+、危险模式 50+
- **`src/security/access_control.{h,c}`**：访问控制（S9/S11/S18）
- **`src/security/secure_channel.{h,c}`**：安全通道（X25519 + XChaCha20-Poly1305 + 防重放）
- **`src/security/sensitive_data.{h,c}`**：敏感数据分级（S14 四档）
- **`src/config/options.{h,c}`**：可选项开关体系（7 底线 + 57 可选 + **隐私保护模式**）
- **`src/update/update_verify.{h,c}`**：更新签名校验（Ed25519）
- **`src/python/permission_gateway.py`**：Python 侧权限闸门（S17）
- **`src/tui/tui_disabled_stub.c`**：TUI 关闭时的降级桩
- Makefile 开关：`ENABLE_TUI`（默认 0）/ `ENABLE_SYSTEM_MHD`（默认 0）

### 修复（Bug Fixes）
- **S1 加密虚假声明**：`encrypted:true` 固定返回 → 改为**能力协商后的真实结果**
- **S6 随机源降级**：3 处 `srand(time^pid)` → **失败即拒绝**（token/验证码/连接码）
- **S16 token 入日志**：完整打印 → **仅前 8 位**
- **crypto_core 致命缺陷**：`crypto_random_bytes` 用 **LCG + 硬编码种子**（序列可预测）→ 改为 getrandom/urandom，失败即拒绝
- **envelope 缓冲区溢出**：KEK 派生用 64 字节哈希写入 32 字节缓冲 → 改定长 KDF + 2 万次迭代
- **S3 文件写入**：4 条路径黑名单（可写 `~/.ssh/authorized_keys`）→ 统一交 `permission_check_file`（含 realpath 规范化）
- **S4 SSRF 补全**：拦截 `169.254.0.0/16`（云元数据）、IPv6 映射、数字型 IP；修正 `172.16-31` 精确范围（原误伤 172.32+）
- **S4 重定向绕过**：`requests.get` 默认跟随重定向 → 改 `allow_redirects=False` + **逐跳校验**
- **S11 CORS**：两处 `Access-Control-Allow-Origin: *` → **校验 Origin**
- **S9 `/api/cmd` 无认证**：新增 IP 分级（localhost/LAN/PUBLIC）+ 限流
- **TUI 依赖**：默认关闭（代码全保留，`make ENABLE_TUI=1` 可恢复）
- **supervisor 链接**：补 `http_client.c`（install_model 下载用）

### 变更（Changes）
- 内部版本 `LN-0.4.4` → `LN-0.5.0`
- `alert_notify.c` / `visiond.c` / `install_model.c`：curl → 内置 HTTP

## [0.4.4] - 2026-09-12（部署链修复 + shell 指令补齐）

### 修复（Bug Fixes）
- **LD_LIBRARY_PATH 污染 → python SSL 不可用（AI 回复空白根因）**
  - `src/core/main.c`：拉起 ai_server 前 `unsetenv("LD_LIBRARY_PATH")` + 优先 `/usr/bin/python3`（可用 `LINGOS_PYTHON` 覆盖）
  - `scripts/bundle.sh` 的 start.sh：**不再全局 export LD_LIBRARY_PATH**（依赖 rpath=$ORIGIN/../lib），仅在缺库兜底时才设
- **sysbin 包残缺**：补齐 `install.sh / start.sh / check_deps.sh / lingos.sh / manifest.json`（此前 deploy 脚本依赖落空）
- **`app search / update / upgrade` 未接线**：`repo_search_command / repo_update_command / repo_upgrade_command` 早已实现却从未接入 `app_dispatch` → 已接入
- **`app daemon` 假占位**（原提示"不支持"）→ 接入真实 `app_daemon_start/stop/is_running`
- **`time_test` 未实现** → 补全真实时钟自检（CLOCK_MONOTONIC/REALTIME/gettimeofday + 单调性验证）
- **插件层从未启用（深坑）**：`plugin_loader` 早已实现却**从未被调用** → 插件化实为空架子
  - ai_server 启动时 `load_plugin_layer()`；插件技能并入 `skill_schemas`
  - `execute_skill` 内置表未命中 → **回退插件**
  - 新增 `plugin_list` / `plugin_reload`（App/Web 统一管理 + 热重载）
  - 新增插件模板 `_plugin_template.py` 与 `README.md`
- **registry 统一注册**（先生 2026-09-05 裁决）：全系统此前仅 `core/state.c` 注册 1 条
  - 新增 `src/registry/registry_builtin.c`：按类目架构注册 16 个系统
    （core/comm/notify/voice/config/data/plugin/security/home/monitor/alert/ai/skill/ai_vision/ocr/weather）
  - `background_init` 在 `registry_init` 后调用
- **🔴 Web UI 数据绑定层整体失效（致命）**：`webui/index.html` 中
  `txt.split('` + **裸换行** + `')` 字符串未闭合 → **整个 `<script>` 解析失败** →
  14 个页面的数据绑定函数**全部未定义** → 页面永远停在静态假数据。
  这正是先生实测「控件不可点 / 数据是虚假的 / 天气是模拟的」的**真正根因**。
  - 已修：`split('\n')`
  - 修复后脚本可解析，`bindSys/bindLog/apiCmd` 等全部生效
- **p-sys 页改为真实绑定**：插件列表 ← `plugin_list`（新增热重载按钮）、
  注册表 ← `registry_list`、系统信息 ← `system_info`（版本不再写死 LN-0.4.3）；
  不可达一律显示 `--` 不模拟
- **AI 识别引擎 AI 侧未接线**：`vision_ai.py`（双路径：文本化/多模态）从未被调用
  - 新增 `ai_vision_ask` 命令（question + image_path）→ 走 `vision_ai.vision_ask`
  - 说明：`ai_vision_detect` / `ai_vision_ocr` 目前仍为**空壳桩**（返回空 data，
    真正检测需监控/YOLO 服务在线）——**未完全实现，此处如实标注**
- **🔴 registry 初始化未置位（既有 bug）**：`registry_init()` **成功路径**未设 `g_initialized`
  → 先生环境已有 `registry/index.json` → load 成功 → 必中
  → 启动时 16 个内置系统注册全部报 `registry not initialized`
  - 已修：成功路径补 `g_initialized = 1`
  - 说明：日志逐项输出**保持原样**（先生 2026-09-12：正常诊断输出，不降噪）
- **check_deps.sh**：补检 `piper`（先生裁语音含 piper）、`websocket-client`、`ssl` 可用性、本体 `ldd` 缺库清单

### 新增（Features）
- **`scripts/lingos.sh`**：简便启停（`start|stop|restart|status|log|ui|doctor`）——自动挑可用 python、`doctor` 一键诊断

### 变更（Changes）
- 内部版本 `LN-0.4.3` → `LN-0.4.4`

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
