# LING OS App 更新日志（CHANGELOG）

版本线：SemVer 2.0.0（主.次.修订）· 包名：LINGOS_<架构>_android_beta_<版本>.apk

---

## [0.2.0] - 2026-08-12（已实施——一批次完）

### 修复（Bug Fixes）
- **工具调用后续轮 HTTP 400**（0.1.9 已知问题）：统一 LLM 调用层重写——消息规范化（tool 消息 content 非空置 `(no output)`；assistant 带 tool_calls 时 content=""）+ 400 自动回退（去掉 reasoning_effort 重发一次）
- **token 用量假 0 落盘**：DeepSeek 流式 usage 真实提取（stream_options include_usage + 流末尾 usage 字段）→ done.usage + token_usage.jsonl 真落盘
- **tool_result 事件名硬编码 "tool"**：改为真实技能名（App 显示工具名）
- **tool 错误提示笼统**：17 类错误分类 + tool_error 事件 + 建议动作
- **50 条硬截断丢主线**：去掉——按 token 预算管理（70% 预压缩后继续累积）
- **错误响应无法定位**：格式错误落盘 /LINGOS/logs/api_debug/（原始请求体 + 服务端返回详情，API Key 脱敏）

### 新增（Features）
- **统一 LLM 调用层（AI-AGENT#7）**：
  - 原生直连不做转换——openai/anthropic 两 adapter，用哪个模型用哪个格式
  - OpenAI 格式：DeepSeek/Kimi/GLM/通义/Compatible API/Ollama（/v1/chat/completions 兼容 tools+reasoning_effort）
  - Anthropic 原生 Messages API（tool_use/tool_result/thinking 流式解析）
  - provider.json 配置化（id/base_url/api_key/model/format/context_window）
  - **模型列表 App 同步显示 + 切换**（provider_list / model_switch / provider_add / provider_remove）
  - 错误分类（400/401/402/403/404/405/413/429/5xx/超时/网络——中文说明+建议）
  - 429/5xx 指数退避重试 1 次
  - 上下文窗口动态获取（显式配置 → 内置映射表 → /v1/models → 无限）
- **上下文引擎**：
  - **双记忆**：重要记忆（AI 自决 importance:high，`[重要]` 前缀）自动注入（≤800 token）；普通记忆 AI 自主调用（协议 AI-AGENT#5）
  - 状态外部化延续：plan.md/notes.md 路径指针注入（上下文放指针不放内容）
  - 70% 预算预压缩（不等超限）+ `context` 事件（App 提示条）
  - 压缩前落盘 /LINGOS/data/session_archives/（AI 可回溯）
  - `context_status` 命令（会话页查询上下文 token 状态）
  - 记忆索引增强：search_keywords / find_important / search_constants（保存重要信息去除杂乱）
  - `session_read` / `session_search` 技能（AI 查看/搜索其他会话——默认允许）
- **工具调用（AI-AGENT#9）**：
  - 17 类错误说明（先生清单 + 空数据 + 被中断）：不存在的参数/缺少参数(列明)/缺少依赖路径/未能找到内容/未能找到包/下载失败/timeout/安装失败/没有那个文件或目录/权限不足/用户阻止/用户拒绝/系统内部错误/非法地址拒绝/不存在的工具/空数据/被中断
  - 错误分类体系：MissingDependency/ExecutionError/AuthDenied/Timeout/InvalidArgs/NetworkError/Unknown 等
  - 工具报错自动查询知识库（不再靠 AI 自觉）
  - 并行结果 tool_call_id 回填
  - schema 注入 A+B：全部技能精简描述 + 核心高频组（~47 个）全量注入
- **提示词精简（E1）**：合并重复段（guide/memory_guide/skills_desc 去重）→ system prompt token 显著下降
- **语音系统（AI-AGENT#8）**：
  - TTS + STT 都要；本地直连/服务端代理可选（连接设置"音频提供商使用服务端代理"开关）
  - 降级链：提供商 → 服务端本地 TTS（espeak-ng/piper，装进安装系统）→ 设备本地 TTS
  - **音频不走 WS**：WS 信令 + HTTP(8088) REST（POST /api/audio/tts、POST /api/audio/stt、GET /api/audio/file——Bearer token）
  - 提供商直连（原生 REST）：ElevenLabs/Deepgram/Azure/MiniMax/百炼/火山/MiMo（讯飞暂不接入）
  - 词组机开放：主机其他服务/设备可调用语音（HTTP REST）
  - 用量统计 voice_usage.jsonl（字符数/时长/次数）+ voice_usage_query
  - 音频 24h 自动清理 + audio_clear 手动清空
  - 自动朗读开关（默认关）/ 连续对话开关（家居默认开）——App 设置
- **App 端**：
  - 新事件适配：context（压缩提示条）/ tool_error（错误卡片红底+建议）/ meta（会话头部）
  - 状态行：model + token 上传↑/下载↓ + 缓存命中 + AI 输出字数 + 已压缩标记
  - 模型切换 UI（主机端模型列表 ActionChip 点击切换）
  - 语音 UI：录音按钮（点击录音→STT→发送）+ AI 消息朗读按钮（TTS 下载播放）
  - manifest 权限补全（FOREGROUND_SERVICE/MICROPHONE/WAKE_LOCK/READ_MEDIA_AUDIO/MODIFY_AUDIO_SETTINGS）
  - 新依赖：record ^7.1.1（录音）+ audioplayers ^6.8.1（播放）

### 服务端配套（先生环境复制 src/python/ 即生效）
- 新增文件：llm_unified.py（统一调用层）、voice_service.py（语音服务）
- 改造：ai_server.py（统一层包装/上下文引擎/错误分类/新命令/HTTP 8088）、skill_handlers.py（memory importance）、memory_retrieval.py（双记忆/索引增强）
- 新增命令：provider_list / model_switch / provider_add / provider_remove / context_status / voice_tts / voice_stt / voice_usage_query / audio_clear
- 新事件：context / tool_error / meta / tts_* / stt_*
- 协议 v2 已更新（AI-AGENT#7/8/9 + 事件清单 + 已知问题 11-15）

### 技术备注
- Ollama 走 OpenAI 兼容端点 /v1/chat/completions（官方文档确认 tools+reasoning_effort 兼容，api_key 任意被忽略）
- 未配置 provider.json 时保留 DeepSeek 直连为默认（向后兼容）

---

## [0.1.9] - 2026-08-12（当前最新）

### 修复（Bug Fixes）
- **对话消息无法发送**：App 发送 `content` 字段、服务端只认 `prompt`——改为 App 发 `prompt` 适配（服务端现状）
- **AI 回复不显示**：服务端 AI 事件包装为 `chat_event`，App 无法识别——新增解包（data 内递归解析 content/thinking/tool/done）
- **仪表盘数据空白**：command_response 的 data 类型不匹配（App 期望 String、服务端发 Map）——兼容两种类型 + status 校验 + 空数据明确提示
- **AI 工具并行调用崩溃**：服务端 `call_deepseek_stream` 缺 `tool_calls_acc` 初始化（NameError）——已补
- **权限/人格设置无效**：服务端命令解析未扁平化 `params` 嵌套（参数全空）——解析后扁平化到顶层
- **左上角三横点击无响应**：Scaffold.of 跨层找不到 HomeShell 的 drawer——改用 GlobalKey + 回调
- **版本号不更新**：build.gradle 硬编码 versionName "0.1.8" 覆盖 pubspec——改为动态 `flutter.versionName`
- **日志重复入口**：AI 配置主页与连接与设置内重复——删主页入口，设置主页独立"日志"入口 + 记录开关

### 新增（Features）
- **AI 配置界面（15 子菜单）**：
  - LLM/MLM 提供商：添加提供商（12 家预填——OpenAI/Anthropic/Compatible/Kimi + 语音 8 家；三段式：列表→选择→密钥配置）
  - Token 完整用量：汇总卡片/时间过滤/按模型分组/明细（服务端 token_usage_query + JSONL 落盘）
  - 权限管理：19 权限 × 5 模式（拒绝/单次/使用中/始终/影子）+ 分组折叠（设备/存储/网络/后台/应用）+ 类级统一授权 + **真实 Android 系统授权**（permission_handler）
  - 记忆管理：时间显示 + 摘要展开 + AI 自动写入开关
  - 会话管理：点击进入对话续接 + 长按多选批量删除
  - 人格：诺克/诺玛选择 + 参数滑杆（服务端 personality_get/set）
  - 技能：68 技能分组 + 风险色标 + 启用开关（启用 ≠ 权限：容器内全权/主机需授权）
  - 额外的 MCP：服务器列表/添加（认证可选）/测试连接/删除（服务端 mcp_add/remove/list/test）
  - Rootfs 本地沙箱管理：多发行版安装向导（Alpine/Ubuntu/Debian）+ 下载进度 + 状态卡片 + proot 路径配置
  - 挂载外部文件：Android 外部存储 + 服务端 /LINGOS 双来源 + 持久化 + 只读/读写
  - 管理服务端文件：/LINGOS 九大目录结构化导航 + 查看
  - 浏览文件：点击查看内容 + 重命名（增强）
  - 通知与后台：预警/灵动胶囊/任务通知开关 + 后台保活/模式 + 电池优化/自启动引导
  - 连接与设置：主机IP/连接密钥/连接状态/连接方式/加密 + 退出登录
  - 外观与偏好：主题（深/浅/跟随系统）+ 强调色动态取色 + 语言 + 消息偏好（全开）+ 隐私统计（默认关）
  - **特权**：Shizuku 检测/授权 + AI 经 adb 执行命令通道（shizuku_api 1.2.2）
- **导航重构**：左上角三横 Drawer 统一入口（设置/HA/预警）+ 底部保留对话/仪表盘；原设置/记忆/文件迁入 AI 配置
- **对话交互**：发送键 ↔ 中断键切换 + "已终止（继续）"重发原文续接（服务端 interrupt 帧 + chat 独立线程）
- **预警适配**：alert_query 返回 events 字段渲染 + 字段映射（description/location/timestamp）
- **打包**：CI 资产加 `LINGOS_` 前缀（arm64-v8a/armeabi-v7a/x86_64/all 四包）

### 服务端配套（ai_server.py——先生环境 cp 即生效）
- 新增 12 命令：token_usage_query / permission_set / permission_list / skill_list_full / skill_enable / personality_set / personality_get / ai_config_set / mcp_add / mcp_remove / mcp_list / mcp_test
- S1/S2 修复（见上）
- websocket_server.c：WS 端口 2939（原 3940 与协议不符）+ interrupt 帧 + chat 转发独立线程

### 技术备注
- CI 桌面端（Linux/Windows）构建失败不阻塞 Android 发布（continue-on-error）
- Windows STL1011 为 CI VS18 环境问题（与代码无关）

---

## [0.1.8] - 2026-08-11

### 修复
- WS 端口 = TCP 端口 + 2（2937→2939 协议约定——曾用错端口导致 WS 未连）
- App 认证前心跳（0x0008 首包拒绝）
- token 重验证弹窗（验证码/登出）

### 新增
- raw socket WebSocket（绕开 HttpClient 101 兼容问题）
- token 加密存储 + 设备绑定
