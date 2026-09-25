# LING OS 服务端更新日志（CHANGELOG）

版本线：发行版 0.4.3（server/app 统一——先生 2026-09-04 规范）· 内部 LN-0.4.3
包名（新规范）：LINGOS_server_linux_v<版本>_<架构>_<allbin|sysbin|plugin>.tar.gz

---

## [0.7.1] - 2026-09-25（hotfix 批次：全量静态扫描 + 安全隐私加固 + App 链路接续）

> 本版 = v0.7.0 部署后的两个 hotfix 批次合并。**重点：全量静态扫描（先生指令）+ 安全与隐私 + App 链路完善**。

### 安全修复（先生重点）
- **命令注入 ×4**：MQTT nook_ask（**远程 RCE**——prompt 元字符过滤）· net_ping/net_curl（AI 可调——
  输入白名单）· camera RTSP / skill 参数过滤
- **文件系统保护**：HTTP /api/files 四端点白名单（仅 /LINGOS+/tmp——原认证后任意系统文件读写删）·
  syscall write/delete 保护路径 · Python 命令面同套保护 · 敏感文件读取拒绝（凭据/密钥/哈希）
- **socket 权限**：ai.sock/auth.sock/embed.sock chmod 0666→0600（防本机越权命令通道）
- **S12 更新验签接入**（system_update_install 前置门：有签名强制/无签名警告）
- **crypto_sign 越界读修复**（32 字节私钥误当 64 字节读）
- **rollback 防数据丢失**（repair_engine——结构校验/原子改名/失败回退）
- 哈希升级 sha1/md5→sha256 ×4 · random→secrets

### 崩溃修复（全量扫描产出）
- **Python 9 炸弹**：get_skill_risk 遮蔽（UnboundLocalError——先生真机复现）· call_syscall 未导入
  （options_list 全挂）· generate_summary 误名 · logger 前移 · session_id/usage_info ·
  skill_loader 闭包 · plugin_loader json · rtsp _running global
- **C 端**：input_filter realloc 内存 ×2 · check_items 泄漏 · active_repair 空指针顺序 ·
  rules_parser 边界 · memory_vector 未初始化 · weather 缓冲溢出
- **日志颜色**（先生报告）：格式串 %s 数不匹配→行尾 RESET 丢失（颜色延续根因）
- **TUI/markdown UTF-8**：多字节字符截断 ×3 · 进度条颜色未应用 ×2

### 诚信化（先生红线）
- **detection_engine**：模拟假检测（假人/猫/火焰）→ **真实 yolo.sock 接线** + 诚实降级
- **audio_input**：rand 假音频 → 真实 arecord 捕获 + 诚实降级；voiced 降级不 abort
- repair_engine.execute_repair 假成功 → 诚实标注

### 功能（批次一）
- **server mode**（新功能）+ **日志系统扩展**（api.log 全通道/device_mod 7 码/不被清除）
- **危机链**：B5 投递五重保障+ACK · B7 危机加速（关思考链）· B3/B4 审批例外+权限全权
- **P3 服务端**：自动记忆管线 · 预警订阅+AI 简报 · 天气联动 · command_list
- **命令面板**（WebUI 第 22 页）· 出口白名单（egress）· dev 日志组

### App
- 命令面板 · 局域网自动发现（连接 2 步化）· 后台推送（危机 critical + 通知桥）
- 服务器模式控制屏 · 天气城市管理 · 危机投递状态 · 首启引导 · 主控台双态（连接后真数据）
- 审批三态（始终允许）· 语音连续对话 · 通知开关接线

### 工具链
- pyflakes/bandit/cppcheck 全量扫描通过 · gcc -Wall 全模块零警告 · 40 Python 模块 import 冒烟

---

## [0.7.0] - 2026-09-24（真机修复包 + server mode + 安全接线 + 危机链核心）

> 本版 = 统一开发计划批次合并：P1（S 系列真机修复）+ P1.5（安全接线）+ P2（server mode 核心）
> + P2.5（危机链核心接线）。**全部改动经静态语法检查**（先生指示：不本地编译、不推 CI 直至全部完成）。

### 新增（Features）
- **server mode（先生设定 2026-09-19 实施）**：
  - 新建 `src/core/server_mode.{h,c}`——只显示日志、不接受输入（控制键唯一：Ctrl-Q/Q）
  - 状态持久化 `server_mode.json`（on 后重启保持）· 日志文件尾随显示（tail 式增量）
  - 终端 raw-ish 模式（捕获 Ctrl-Q——关闭 IXON 流控）· 退出恢复原终端设置
  - **危机时严禁退出**（读 crisis_state.json——铁律：一切行为为人身安全让路）
  - `server mode on/off/stop/status` shell 命令 + `startup mode=server` 启动联动
  - Python 命令面（客户端）：`server_mode_status / server_mode_stop / server_mode_on`
    ——停止走标志文件通道（C 端 shell 循环 + server mode 循环双检测）
  - 服务器模式包（package_mode）：常规关闭无效（off 被拒——停止仅 `server mode stop`）
- **出口白名单（先生裁决 S2 / OWASP LLM06 §3）**：
  - 新建 `src/net/egress.{h,c}`——本机/局域网放行、169.254 云元数据拒绝、公网仅清单
  - 内置清单：生命线（wolfx/usgs/nmc）+ 天气 + LLM 常用 + 语音 + 搜索 + 更新 + 通知
  - 用户扩展：`/LINGOS/system/config/egress_allowlist.json`（domains 合并/通配/enabled 覆盖）
  - 接入点：http_request / http_download / https 请求（hs_request）全覆盖
- **开发调试日志（P2 核心）**：
  - options 新"开发"组（`dev.debug_log` / `dev.api_log` / `dev.prevent_clear`——默认开）
  - 四级判定链（main.c apply_dev_log_policy）：内部变量 build_channel > 版本号（0.x 开）> 手动关
  - 落地：log_set_global_level 动态控制（文件全量写入不受影响）
- **危机链接线（§2B · P2.5 核心）**：
  - B3 **审批链危机例外**（authorization_service：危机中自动批准、跳过 Y/N——audit-only）
  - B4 **权限危机全权模式**（permission_gateway：危机中一切技能放行——audit-only）

### 修复（Fixes · S 系列真机问题全修复）
- **S0-1 部署链断裂**（App 命令大面积 Unknown 根因）：
  - install.sh 补 Python 全量部署（python/server/*.py → bin/ + plugin 子目录）
  - lingos.sh start 自动同步（cmp 对比——幂等）+ main.c fallback 全量化与 mtime 重同步
- **S0-2 WS 消息截断**（先生报告根因）：websocket_server.c 引号扫描
  统一为 `json_scan_str_end`（第一个未转义引号）——替换 9 处 strchr/strrchr
  （strrchr 会吞掉 JSON 尾巴 → "AI 只收到 H","session_id":... 的修复）
- **App 服务器版本不更新**（先生报告）：system_info 响应补 version/internal_version
  （C 端 syscall_handler + Python cmd_system_info 双端——此前响应缺字段）
- **S1-1 唤醒词刷屏**：模拟检测（80/120 次假报）默认禁用——仅 LINGOS_WAKEWORD_SIM=1 测试
- **S1-2 守护残留**：子进程 fork 统一 `child_daemon_prepare`（PR_SET_PDEATHSIG 内核级 +
  父死复查）+ 主程序退出 `terminate_children`（pid 文件 TERM→KILL 双保险）
- **S1-3 终端污染**：子守护 stdout/stderr 重定向到 /LINGOS/log/<name>.log
- **S1-4 registry 竞态**：lingos.sh 等待改"真实 connect 测试"；main.c 新增
  `wait_registry_connectable`（10s）；Python load_skill_schemas 重试 6 次
- **S1-5 冷启动误判**：ensure_ai_server_running 首轮窗口 3s→8s
- **S2-1 visiond 崩溃循环**：无摄像头 → 视觉待机（30s 重试保活）不再 abort
- **S2-2 venv 检查误报**：降级为非计数警告（不再触发"未找到匹配策略"修复噪音）
- **S2-3 registry 子目录**：fs_layout 补建 registry/{builtin,custom,store,skills/*} + core
- **S2-4 YOLO 模型损坏**：加载前完整性预检（<1MB 视为损坏）+ 失败自动重下自愈
- **S3-1 ready 生命周期**：停止/退出时删除 run/ready（防过期"服务=1"显示）
- **S3-3 保存告警降级**：security_config_save 未加载时 ERROR→DEBUG（正常早退场景）
- **S3-4 预警标题 (null)**：类型数组补全（HEALTH=7/SECURITY=8 越界 NULL 根因）+ 范围检查
- **P1.5 安全接线**：
  - **CORS**：api_routes.c 不再写死 `*`（接入 access_cors_add_headers 校验 Origin）；
    monitor_service.py 改本机/内网回显（原 `*`）
  - **限流补线**：/api/files 与 /nook/ask 补 access_rate_allow（此前仅 /api/cmd+webhook）
  - **S6 fail-closed**：permission.c 随机源不可用拒绝（删除 rand() 降级——安全底线违规修复）
  - **S16 铺开**：token 打印全脱敏（Add/Remove——此前仅 GenToken 一处）

### 其他
- 版本统一 0.7.0（Makefile + version.c + CI build.yml）
- 复核：全部改动文件 gcc -fsyntax-only 通过 + Python py_compile 通过（20 文件）

### 批次二增补（同批实施 · 2026-09-24 深夜）
- **API 日志 + 设备修改日志（P2-B）**：
  - 新建 `src/lib/api_log.{h,c}`——api.log（server mode 专属尾随查看）+ device_mod.log（审计）
  - API 日志 **全通道 tap**：HTTP（请求+响应）/ WS / TCP / webhook（随 HTTP）/ socket（daemon）
    / UDP 发现 / Python（handle_client + _reply 双端）——仅 server mode 可查看（双文件尾随）
  - 设备修改日志 **7 码两条式**（ADD/MOD/DEL/MOV/REN/COPY/APPEND——请求条+完成条；req_id 关联）
    隐私：敏感仅记长度（"摘要或长度"）
  - **"不被清除"落地**：日志目录删除拦截（C syscall + Python 双端——`log_path_protected`）·
    512MB 归档（rename 保留——非删除）· 防关机（逐行 append+flush）· 防清屏（dev.prevent_clear）
- **危机链（P2.5 剩余）**：
  - 新建 `src/python/crisis_delivery.py`——**生命线投递五重保障**：
    ① 多通道并行（WS/ntfy/本地声音 espeak/HA 声光）② ACK 强制（30s 重推循环）
    ③ 离线能力（本地 bell+TTS 不依赖网络）④ 断网兜底（重推即兜底）⑤ 延迟指标
    （crisis_delivery.jsonl——各通道时延 + ACK 延迟）· 5 分钟未确认升级通知联系人（可配）
  - **B7 危机加速**：危机进行中 LLM **关闭思考链**（llm_unified 双路径——"关闭一切减速项"）
- **P3 服务端四件**：
  - 新建 `src/python/memory_pipeline.py`——**自动记忆管线**（对话→事实抽取→去重→分级写入；
    启发式 + 可选 LLM；受 `ai.auto_memory` 选项控制）
  - 新建 `src/python/alert_subscription.py`——**预警级别订阅 + AI 简报**（L2+ 过滤推送；
    简报聚合（LLM 优先/模板降级）+ 冷却 + 最小条数）
  - 新建 `src/python/weather_link.py`——**天气↔预警联动**（高温/低温/暴雨/大风阈值 →
    预警事件广播 + 通知；冷却防重复；周期检查线程）
  - `command_list` 命令（命令面板数据源——ext 82 + 技能 + 核心）
- **命令面板（三端同构）**：
  - WebUI 新增"命令面板"页（第 22 页——搜索/渲染/执行/结果）· App 新增命令面板屏
- **部署三件**：**统一版本源**（VERSION 文件——Makefile/CI 单一事实源）·
  **venv 瘦身**（bundle.sh——只装 requests/websocket-client/tiktoken/pillow；
  原 paddle 全套 ~1GB 白装移除——选项 A）· 安装清单对齐
- **App 侧**：局域网自动发现（UDP——连接 2 步化）· 后台推送（crisis critical 通知 + 预警 L2+）·
  命令面板屏 · App 版本 0.7.0+32

---

## [0.6.2] - 2026-09-24（接线批次 + 启动链路完善）

### 新增（Features）
- **生命线数据源真实化（含 TLS 栈新建）**：
  - 新建 `src/net/https_client.{h,c}`——libssl 直连（USGS/EEW 均为 https-only，
    原纯 socket 走 http 实际不可达——301 静默失败根因修复）；二段式证书策略
  - **EEW 秒级四源接入**（wolfx：CENC/SC/CWA/JMA——中国地震预警网/四川/台湾/日本，实测真实数据）
  - 台风源重写：**NMC 国家气象中心真实数据**（原「编造默认事件」删除；实测台风"杜鹃"）
  - CN_WARNING 编造清除（改真实解析用户配置源，失败=诚实空）
  - 中国源识别补全（CENC/CWA/SC-EEW/NMC——merge 优先级修复）
- **webhook HTTP 路由**（`POST /api/webhook/<id>`——注册表此前"只写不接"）
- **ai_vision 三命令真实接线**（detect→yolo.sock / ocr→8892 / calibrate→8893；
  引擎未运行=明确报错，不再空数组假成功）
- **restream → go2rtc 集成**（PUT/DELETE /api/streams；不可用时诚实标注）
- **home_ext → HA 实体控制桥**（场景/实体经 HA service call 落地真设备）
- **启动链路完善**（对齐 systemd/K8s 主流实践）：
  - **单实例锁三保险**（主程序 flock + supervisor 自锁 + 脚本全链预检——
    日志实证一天 4 次端口冲突的根因修复；退出码 75 联动）
  - **supervisor 接线上岗**（lingos.sh 监督者优先；崩溃自动恢复/心跳/限流真正生效；
    子进程相对路径隐患修复）
  - **修复模式死 BUG 修复**（运行脏标记机制——崩溃/断电/强杀下次启动必检出；
    此前异常标记被覆盖导致修复模式永不触发）
  - **服务顺序修正**（生命线 alertd 先行；AI 软降级——失败不再终止系统）
  - **registry.sock 竞态修正**（ai_server 启动前等注册表就绪）
  - **启动就绪报告**（到 Shell 前打印服务/端口一览表 + 写 /LINGOS/run/ready）
  - **心跳增强**（JSON 格式携带服务状态；超时 180s→60s）
  - **服务守护线程**（lingosd 5s 检查+自动重拉；aux 守护 30s 检查+软拉回）
  - **指数退避**（重启 3/6/12/24/48s 封顶 60s + 稳定 300s 复位）
  - **argv 框架**（--fast / --safe / --diagnose / --version / --help）
  - 配置单载收口 + SIGUSR1 误杀防护 + supervisor 优雅 TERM

### 修复（Fixes）
- `cron_add` 追加式修复（原整表覆盖——第二次添加抹掉第一条）
- `update_auto_check` 接线重写（原直连死地址；现走本机 update_check 真实源 + 启动自动运行）
- `repo_client` 死地址清除（未配置=明确提示，不再打死域名）
- 影子模式 C 端刹车接线（`shadow_mode` 体系此前零调用者）
- Qt：版本动态化（读 /LINGOS/version）+ 清除"加密传输"误导声明
- WebUI：会话页/视觉页静态假数据清除（空态，不模拟）

### 变更（Changes）
- Makefile/CI 链接 libssl（soname 22.04~25.10 稳定）；CI 增 libssl-dev
- 内部版本 LN-0.6.1 → LN-0.6.2

### 验证
- 四文件完整编译（main/supervisor/exit_status/repair_mode → obj 0 错误）
- test_exit_status.c 12/12 PASS（崩溃检出/干净退出/信号停止/连续崩溃/旧版兼容）
- 单实例锁行为实测（阻塞+读 PID+退出码 75+释放后成功）
- 数据源 harness 实测：EEW 四源 + NMC 台风 + USGS 全真实数据
- 脚本残留检测实测（假 lingosd 检出+自动清理）

---

## [0.6.1] - 2026-09-13（S1 应用层加密落地 + §2B 危机全权响应 + 影子模式）

### 新增（Features）
- **S1 应用层加密（TCP 通道真实落地）**——审计后的「零调用」代码本次真正接线：
  - `connection_handler.c`：新增 MSG_KEY_EXCHANGE(0x0B) 密钥交换（caps + X25519 公钥 + salt）；
    加密会话逐帧 AEAD（发送端加密/接收端解密/失败拒绝）；会话销毁挂 sc_destroy
  - `secure_channel.c`：修复**方向字节缺失**（双向 nonce 撞车风险）——新增 `sc_set_direction`
    （客户端发送=1/服务端发送=2）
  - `crypto_core.c`：修复**弱公钥检查逻辑颠倒**（crypto_verify32 语义误用——原实现拒绝
    所有正常握手、放行全零弱密钥；该路径从未被调用，测试时才暴露）
  - **端到端实测通过**（TCP 2937 全链：认证→密钥交换→加密心跳→加密 ACK→明文拒绝）
  - 新增测试：`tests/test_secure_channel.c`（9 项）、`tests/test_e2e_encrypt.c`（12 项）
  - KDF 交叉验证：与 Python hashlib 标准一致（7 组）+ 与 C 实现一致（2 向量）
- **§2B 危机全权响应（代码级落地）**：
  - 新增 `crisis.py`：危机判定（确定性——fire/pollution/earthquake/intrusion/gas/water/sos）
    + 类别动作表（火灾开门/污染关门/地震开门/燃气开窗…——可编辑 crisis_actions.json）
    + 10 秒决策启动窗口（动手即解除）+ 全程审计（crisis_audit.jsonl）+ 人身安全让路铁律
  - `ai_server.py`：告警端点自动危机判定；危机期权限检查转 audit-only（全权）；
    工具动手标记；系统提示词注入危机段；crisis_trigger/status/resolve/ack 命令
- **影子模式（权限三态）**：`permission_gateway.py` deny/shadow/allow 三态；
  影子模式返回结构正确的空数据（不报错不泄露）；`ai_server` 接入拦截
- **S5 API Key 加密**：`llm_unified.py` provider.json 的 api_key 经 envelope 加密存储
  （api_key_enc "v1:<hex>"）；读取时解密；daemon 不可用时明文+诚实标注（key_plain）
- **S14 HA 令牌加密**：`ha_integration.py` ha_config.json 的 token 加密存储（同 envelope）
- **S15 备份加密**：`backup.c` 备份时敏感文件（provider/ha_config/passwd/device.key）envelope
  加密为 .enc；还原时解密；backup.key 不入备份（泄露仍受保护）
- **crypto_encrypt / crypto_decrypt / notify / permission_list 新 syscall**
  （前者错误致）修复路径：缺 `os` 导入、S5/S14 往返实测通过
- **MCP 工具接线**（修复「注册的服务器无法被 AI 调用」）：
  - 新增 `mcp_client.py`：JSON-RPC over HTTP（initialize → tools/list → tools/call）
  - `ai_server`：MCP 工具并入 AI 工具表（命名 `mcp__<server>__<tool>`）+
    执行路由；`mcp_test` 升级为真实握手（工具发现）
  - Mock 测试全链通过（initialize/发现/调用——含中文）
- **WebUI 控制台真实化**：TOKEN 趋势接线（真实 6h 聚合折线）/ SYSTEM EVENTS
  接线通知中心 / ALERT 卡接线预警列表（清除全部静态假数据）

### 验证
- S1 E2E：TCP 全链 12 项全过（含「明文帧被拒」AEAD 防线）
- S5/S14 加解密往返：UTF-8（含日文/emoji）实测一致
- 全部改动文件语法检查通过；C 端仅增量编译所需目标（lingosd 80s）

---

## [0.6.0] - 2026-09-13（功能批次：技能链/GUI链/审批链/权限链全修 + 三内核守护全量打包）

### 修复（Fixes）——四座大山（深挖 P0 核心）
- **技能链 4 断点全修**（"无法拉取 skill"根因闭环）：
  - `registry.c`：metadata **持久化修复**（save 写入 / load 恢复——技能描述/参数不再丢失）
  - `registry_skill.c`：新增 `registry_skill_write_index()` 索引导出器（**index.json 此前无写入者**——现从内存+磁盘双源导出）；`load_all` 接入索引导出；**扫描器支持技能包目录**（`<name>/skill.json`——此前只认平铺文件）
  - `background_init.c`：`registry_skill_load_all()` 接线（**此前无调用者**——技能目录从不加载）
  - `file_integrity.c`：索引路径分歧修复（`/skills/` → `/registry/skills/`，防与读取方/写入方错位；防误擦除）
  - `skill_store.c`：装/卸技能即时刷新索引（AI 侧即时可见）
  - `skill_loader.py`：Python 执行器**多策略解析**（模块导入 → 5 个技能目录文件探测；入口兼容 run/handle/execute/main）
  - `skill_install.py`：**双规范扫描**（SKILL.md / skill.json）+ `/skills/enabled` 纳入扫描
  - `ai_server.py`：新增 `skills_reload` 命令（热重载注册表技能+自定义技能）
- **GUI 交互链全修**（三处全断闭环）：
  - `ai_server.py`：工具结果 **gui_interaction 拦截** → 按协议转 `gui_ask/gui_notify/gui_open_url/gui_share/gui_location/gui_clipboard` 事件推 App（此前 0 命中——用户永远看不到 AI 提问/通知）；工具结果改写防重复调用
  - App：6 个 GUI 事件全处理 + 提问弹窗（选项/输入→回传 AI）+ 状态驱动渲染
- **审批链全修**（60s 必超时闭环）：
  - `ai_server.py`：新增 `auth_respond`（App 审批回执 → auth.sock 写入）/ `auth_pending`（重连恢复待审批入口）
  - App：`auth_request` 事件处理 + **审批卡片弹窗**（批准/拒绝 → auth_respond 回执）
- **权限链全修**（11 个高危技能永久死锁闭环）：
  - `permission_gateway.py` 重写权限查询：**直读 `/LINGOS/system/config/ai_permissions.json`**（与 App 设置同一事实源）→ 用户设置即时生效；未配置项按域默认（操作类放行[受风险分级+审批+审计约束]，隐私类跟随 UI 权限）
  - `syscall_handler.c`：daemon 补 `permission_list` 操作（此前缺失 → 网关查询恒失败 fail-safe 拒绝）
- **人格切换断链修复**：`_resolve_personality_text()` 桥接（personality.json → 专用文件 → **内置 nook/noma 文本兜底**）——切换立即生效（此前两字段无桥）
- **通知桥缺口修复**：`syscall_handler.c` 补 `notify` 操作（WS 广播达 App[弱符号] + 轨迹落盘——Python 3 处调用此前无人实现）
- **提醒投递修复**：`ai_reminder.c` 触发时 WS 广播+落盘（此前仅终端打印——用户收不到）
- **MHD 内置实现崩溃修复**（严重）：`mhd_compat.c` 响应**引用计数**（对齐真实 libmicrohttpd 语义）——修复「queue 后 destroy 即真释放 → 发送时 use-after-free + conn_free 二次释放 → SIGSEGV」；沙箱实测 `GET /system/health` 由崩溃(139) → **HTTP 200 稳定**
- **状态栏格式串修复**：`log_extra.c` 格式与实参 12 槽全对齐（0.5.2 修复不完整——musl 下 `%s` 收到 int 0 段错误；glibc 下显示垃圾数字）
- **全捆模式跳过过时依赖检查**：`main.c`（`LINGOS_BUNDLED=1` → 不再检查/尝试安装 `lib*-dev` 开发包——0.5.0 起全捆自带运行库；原每次启动慢且必失败）
- **Python 部署清单修复**：Makefile `install_python_script` 固定清单 → **全量复制**（原缺 `paths.py`/`permission_gateway.py`/`skill_install.py` 等 → ai_server 启动即 `ModuleNotFoundError`）；**plugin 子目录补齐**（主包/sysbin——修 `plugin_loader` 找不到）

### 新增（Features）
- **三内核守护全量打包**：`lingos_alertd`（预警生命线）/ `lingos_visiond` / `lingos_voiced`
  - Makefile TARGETS 默认全量构建；bundle.sh 四处清单（主包/库收集/install.sh/sysbin）
  - `main.c` 新增通用守护拉起器（软启动——缺失不阻塞主程序；防旧包兼容问题）
- **规则引擎接线**（三件套）：`rules_engine_start_watchdog()` 周期评估线程 + `lingosd` 启动接线 + shell `rule`/`rules` 命令分发（`rules_dispatch` 此前无调用者）
- **假技能真实化**（6 个）：
  - `typhoon_predict` → **NMC 国家气象中心实时数据**（台风列表/路径/官方预报机构段——零编造）
  - `vision_locate` → **查询真实视觉库** `/LINGOS/data/vision/vision.db`（SQLite）
  - `rule_query` → 读真实规则存储 `rules.json`
  - `defense_mode` → 读真实防御配置 + 引导系统命令
  - `perm_set` → **真实写入权限存储**（与 App/网关同一事实源）
  - `voice_command` → 诚实状态查询（voiced 守护状态/唤醒词——不再假 executed）
- **WebUI 真实化**：端口页（`port_list` 实测探测）/ 更新页（`update_check` 真实检查 + 重试按钮）/ **AI 对话页全重写**（清除永久假对话 → 真实 WS 流式：token 认证/流式渲染/工具行/中断，App 同款协议）
- **`port_list` / `update_check` / `skills_reload` / `auth_respond` / `auth_pending`** 新命令（App/Web 统一命令面）

### 验证
- 沙箱全量编译：六二进制（lingos_linux/lingosd/supervisor/alertd/visiond/voiced）**0 错误**
- 端到端冒烟（沙箱）：
  - lingosd 直启 → HTTP 200（health 真实数据）+ WS 101 握手 + 认证帧处理 ✓
  - alertd 真实告警输出（磁盘阈值/USGS/台风源）✓
  - 技能链闭环实测：技能包放入 custom → registry.json 记录（含 metadata）→ **index.json 真实导出** ✓
  - ai_server 启动成功（paths 修复后）+ plugin_loader 导入成功 ✓

---

## [0.5.2] - 2026-09-12（先生实测三项修复 + 版本动态化）

### 修复（Fixes）
- **TCP 通道命令被拒**（先生实测：`system_info` / `balance_query` / `sync_full` / `vision_config_get` / `weather_current` / `options_list` / `permission_list` 全部 unknown command）：
  - `connection_handler.c`：未知命令统一转发 `ai.sock`（与 WS 通道 `ws_forward_command` 行为一致）
  - 收发缓冲 8192 → **16384**（转发响应更从容）
- **端口无人监听兜底**（先生实测：`127.0.0.1:8080 拒绝访问` / App WS 自动连接失败）：
  - `api_core_init`：跳过前**实测 WS/HTTP 端口**；lingosd 在但端口未监听 → 主进程**兜底启动**（防「PID 在、服务死」窗口）
  - `[API] Ready` 消息改用实际端口（原硬编码 2939/8080）
- **内部版本号不迭代**（先生报告）：
  - `ai_server.py` 启动日志（原 LN-0.4.3）→ 动态读 `/LINGOS/version`
  - `lingosd.c` / `api_routes.c` / `registry_builtin.c` / `monitor_service.py` 硬编码 → 动态
  - `version.c` 回退值 → LN-0.5.2；Makefile VERSION → LN-0.5.2；CI VERSION → 0.5.2

### 验证
- 沙箱全量编译：`lingosd` (3.17MB) + `lingos_linux` (3.81MB) **0 错误**
- 冒烟实测：lingosd 直启 → **WS 2939 / HTTP 8080 均正常监听** ✓

---

## [0.5.1] CI 提速（2026-09-12 · 补）

### 改进（CI）
- **arm64 构建改原生 runner**：GitHub 官方 `ubuntu-22.04-arm`（公开仓库免费）替代 qemu 模拟
- 构建时长：**25-30 分钟 → 约 5-8 分钟**（qemu 下 apt/pip/编译全部在模拟层；原生后与 x86_64 同速）
- glibc 基线保持 **2.35**（22.04~25.10 兼容承诺不变）；x86_64 保持 ubuntu-22.04

---

## [0.5.1] - 2026-09-12（22 项缺失功能全部补齐 · 批次2~5）

### 新增（Features）
**批次 2 · 智能家居（对标 Home Assistant · 8 项）**
| # | 功能 | 实现 |
|---|---|---|
| M1 | **设备自动发现** | mDNS(5353) + SSDP(1900) + MQTT Discovery 记录 |
| M2 | **场景 Scene** | 多实体状态快照/一键恢复（自动触发自动化） |
| M3 | **区域 Area + 标签 Label** | 实体组织（层级 area + 多标签） |
| M4 | **能源管理** | 功率/电量采样 + 成本计算（可配电价） |
| M5 | **在家/离家 Presence** | 状态机 + 变更触发自动化 |
| M6 | **地理围栏 Zone** | Haversine 距离 + enter/leave 事件 |
| M7 | **Webhook** | 外部触发 → 执行绑定动作（scene/presence/notification） |
| M8 | **蓝图 Blueprint** | 参数化模板 + `${param}` 占位替换 |

**批次 3 · 监控/NVR（对标 Frigate · 7 项）**
| # | 功能 | 实现 |
|---|---|---|
| M9 | **移动侦测前置** | 降采样帧差 + 冷却 + 前置缓冲 → **有运动才送 AI（省 90% 算力）** |
| M10 | **时间线回放** | 事件索引 + 按小时聚合密度条 |
| M11 | **对象录像保留** | 含对象录像延长保留期（默认 7→30 天） |
| M12 | **存储管理/循环覆盖** | 过期删除 + 超限删最旧（优先删无对象） |
| M13 | **ONVIF** | WS-Discovery 探测 + 端口/服务探测 |
| M14 | **RTSP 重流** | 单上游连接多消费者（自动分配端口） |
| M15 | **动态合成视图** | 多路布局计算（自动算行列/单元格坐标） |

**批次 4 · AI（4 项）**
| # | 功能 | 实现 |
|---|---|---|
| M16 | **文档问答 RAG** | 接线 `embed_service`（此前死代码）；无向量时**退化关键词检索** |
| M17 | **知识库上传** | 文档提取 + 语义分块 + 可选向量索引 |
| M18 | **会话分享/导出** | Markdown / JSON / 纯文本 + 可分享文本（截断保护） |
| M19 | **图片生成** | 复用图像模型；**未配置时明确指引（不伪造）** |

**批次 5 · 组织/体验（3 项）**
| # | 功能 | 实现 |
|---|---|---|
| M20 | **通知中心** | 统一收件箱 + 已读/未读 + **免打扰时段**（critical 仍推送） |
| M21 | **仪表盘自定义** | 卡片布局持久化（span/order 规范化 + 多 profile） |
| M22 | **媒体播放器控制** | play/pause/stop/next/volume/mute/seek/play_media |

### 实现方式
- **`src/python/ext_dispatch.py`**：**统一扩展分发器**（82 个命令，映射表驱动）
  - 按函数签名自动过滤参数（避免 unexpected keyword）
  - 位置参数兜底、异常隔离
- 4 个新模块：`home_ext.py`(727行) / `nvr_ext.py`(612行) / `ai_ext.py`(515行) / `ux_ext.py`(387行)
- **中文检索修复**：`\w+` 无法匹配中文 → 改为**英文按词 + 中文 bigram**（无需 jieba）

### 验证
- Python 语法 9/9 ✓
- **端到端 18/18 命令执行成功** ✓
- 扩展命令总数 **82** ✓

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
