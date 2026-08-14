# LING OS 0.2.0 方案（定稿）——模型/上下文/工具/语音 全面优化

> 架构铁律（先生确认）：**AI 调用均在主机端（ai_server.py）**，App 为纯前端。
> 服务端负责全部 AI 逻辑；App 消费流式事件 + 显示 + 配置同步。
> 实施方式（先生决策）：**一批次完**——✅ **2026-08-12 已全部实施完毕并推送 GitHub（commit 131d421）**；先生环境复制 src/python/ + 装 App 0.2.0 验证。

---

## 一、模型调用统一层（AI-AGENT#7）

### 1.1 原生直连，不做转换
- 统一入口 `call_llm_stream(messages, tools, provider)` → 按 `provider.format` 分发 adapter
- **`format: "openai"`** → `POST {base}/chat/completions` 原生 OpenAI 格式
  - DeepSeek / Kimi / GLM / 通义 / Compatible API / **Ollama**（官方确认 /v1/chat/completions 兼容 tools + reasoning_effort，api_key 任意被忽略）
- **`format: "anthropic"`** → `POST {base}/v1/messages` 原生 Anthropic Messages 格式（tool_use/tool_result/thinking_content）
- 对外事件统一（thinking/content/tool_calls）；对内各 adapter 原生解析响应体

### 1.2 配置化 provider.json（App ai_config_set 写入）
```json
{
  "id": "deepseek",
  "name": "DeepSeek",
  "format": "openai",
  "base_url": "https://api.deepseek.com",
  "api_key": "sk-...",
  "model": "deepseek-v4-flash",
  "context_window": null,
  "supports_tools": true,
  "supports_reasoning": true
}
```
- **模型列表同步**：主机端配置的模型 → App 同步显示并可切换（新命令 `model_switch`）
- 未配置任何提供商 → 保留 DeepSeek 直连为默认（现有行为不破坏）

### 1.3 格式错误调试（先生要求）
- 一旦格式错误：打印**发送给服务端的原始请求体** + **服务端返回的详细内容**（一般服务端会说明错在哪）
- 落盘 `/LINGOS/logs/api_debug/`（时间戳分文件，API Key 脱敏）
- 这是排查 400 根因的核心手段

### 1.4 错误分类与重试
- HTTP 400/401/402/429/5xx/超时/网络——每类中文详细说明 + 建议动作
- 429/5xx 指数退避自动重试 1 次（可配）

### 1.5 上下文窗口
- 内置映射表（deepseek-128k / kimi-128k / glm-128k…）→ 尝试 `/v1/models` URL 获取 → **默认无限**（不截断，靠压缩管理）

---

## 二、上下文构建与 token 减少（AI-AGENT#9）

### 2.1 双记忆（先生决策）
- **重要记忆自动注入**：AI 写入记忆时自决 `importance`（high=自动注入 / normal=自主调用）
- 普通记忆：AI 自主 memory_search/memory_write（保持协议 AI-AGENT#5 手动原则）
- 自动注入限预算（≤800 token/轮）

### 2.2 状态外部化（延续 AI-AGENT#4）
- 长任务上下文只放 plan.md/notes.md **路径指针**（几十 token）
- 每轮注入：`当前任务：/LINGOS/data/agent_tasks/xxx/plan.md（详情请 read_file）`

### 2.3 分层上下文（四段）
- 静态 system（精简后 ~1500）+ 记忆（Top-K ≤800）+ 历史摘要（压缩后）+ 当前轮
- 达预算 70% 预压缩（不等超限）；输出预留 4096 token
- 压缩触发 → 发送 `context` 事件 → App 显示"上下文已压缩"提示条
- **压缩保存**：旧消息落盘 `/LINGOS/data/session_archives/`（AI 可回溯读取——防摘要丢失关键信息）
- 去掉 50 条硬截断 → 按 token 预算管理
- 实时用量上报：done.usage（prompt/completion/total/context_tokens/compressed）

### 2.4 记忆增强
- 索引 / 关键词搜索 / 常量查找——保存重要信息、去除杂乱

### 2.5 会话查询技能
- 新技能 `session_read` / `session_search`——AI 可查看/查询其他会话内容（**默认允许**）

---

## 三、工具调用与错误（AI-AGENT#9）

### 3.1 工具错误必须说明（先生列的 15 类 + 补充）
| # | 错误 | 说明要求 |
|---|---|---|
| 1 | 不存在的参数 | 指出哪个参数不存在 |
| 2 | 缺少参数 | **列明缺哪些参数** |
| 3 | 缺少依赖路径 | 缺哪个路径 |
| 4 | 未能找到相关内容 | 搜了什么没找到 |
| 5 | 未能找到包 | 缺哪个包 + 安装命令 |
| 6 | 下载失败 | 失败原因 |
| 7 | timeout | 哪个操作超时 |
| 8 | 安装失败 | 失败原因 + 建议 |
| 9 | 没有那个文件或目录 | 具体路径 |
| 10 | 权限不足 | 缺什么权限 + 如何授予 |
| 11 | 用户阻止了你的操作 | 用户拒绝说明 |
| 12 | 用户拒绝了你的操作 | 同上 |
| 13 | 系统内部调用错误 | 含具体内部错误（如暂未实现） |
| 14 | 工具访问非法地址被系统拒绝 | 具体地址 |
| 15 | 不存在的工具 | 提示可用工具 |
| 16 | **空数据** | 工具返回空要说明（非"成功"） |
| 17 | **工具被中断** | 说明中断原因/位置 |

### 3.2 错误分类体系
`MissingDependency / ExecutionError / AuthDenied / Timeout / InvalidArgs / NetworkError / Unknown`

### 3.3 联动
- 工具报错自动查询知识库（query_knowledge_base——不再靠 AI 自觉）
- 并行工具结果按 tool_call_id 对应回填（防错位）
- 循环上限：**无限**（保持 0——靠目标保护+状态外部化防失控）
- tool_result 事件传**真实技能名**（修硬编码 "tool"）

### 3.4 工具 schema 注入 = A+B（先生裁决）
- **A. 全量精简**：68 技能描述精简（名称+一句话用途+必要参数）→ 预计 15-20k → 6-8k token
- **B. 分层**：核心高频组（记忆/知识库/文件/系统 ~15-20 个）全量注入 + `skill_group_list` 查询入口 + `list_skills` 兜底
- 调研结论：主流（OpenAI/Anthropic/业界）均全量注入+分层子代理，**无关键词动态注入**（漏检风险）——故弃用

---

## 四、提示词优化（E）

- system prompt 精简合并重复段（guide 与 skills_desc 工具要求重复、memory_guide 与 guide 记忆部分重复）→ ~2500 → ~1500 token
- 静态段缓存（identity/region/core/sys/thought/interaction 不变复用）
- 人格/助手文件保持直接拼接（不改变注入方式）

---

## 五、语音系统（AI-AGENT#8）

### 5.1 能力与运行方式
- TTS + STT 都要
- 用户可选：**本地直连** 或 **服务端代理**（连接设置新增"音频提供商使用服务端代理"开关）
- **词组机开放**：连接到主机的其他服务/设备也能调用语音能力（主机=语音网关）
- 模型同步：主机端语音配置 → App 同步显示
- 降级链：无配置/代理失败 → **服务端本地 TTS**（espeak-ng + piper，加入安装系统向导）→ 服务端无 TTS → **设备本地 TTS**（android-speak）
- 添加音频提供商时**提醒**（走服务端代理 / 本地直连——隐私与路径提示）
- 自动朗读：默认关，可开启
- 密钥：服务端代理时存主机 /LINGOS/system/config/（隐私第一，不落 App）

### 5.2 音频传输（先生问——不走 WS）
```
WS(2939)  只传信令：tts_request / tts_result(file) / stt_result(text) / 错误
HTTP(8080) 音频数据：POST /api/audio/tts（JSON→音频流/落盘）· POST /api/audio/stt（multipart 上传）
认证：Authorization: Bearer <token>（复用 WS token）
```
- 理由：60s 录音 base64 ≈ 400KB+ 会撑爆 WS JSON 帧；HTTP 支持上传/下载进度、断点续传；Dio 原生支持

### 5.3 TTS 播放（先生决策：都开发）
- **整段文件播放** + **流式播放**——用户设定（"TTS启用流式"开关）

### 5.4 语音交互模式
- 按情景决定：家居等问题**默认连续对话**；App 可设定是否启用连续对话
- 一次性模式（按住说话→识别→回复→朗读）也保留

### 5.5 其他
- STT 语言：跟随系统语言设定
- 音频文件：24h 自动清理 + 手动清空入口（/LINGOS/data/audio/）
- 用量：独立 `voice_usage.jsonl`（字符数/时长/次数）
- 讯飞专有协议：**暂不接入**（WebSocket + 三凭证，适配成本高）
- App 需声明 RECORD_AUDIO 等权限（先生指出：**部分权限未在 manifest 声明 → 系统设置无法授权**——0.2.0 补全）

---

## 六、App 联动清单

### 6.1 新事件（App 需适配）
| 事件 | 方向 | 内容 | App 行为 |
|---|---|---|---|
| `context` | 服务端→App | 压缩/摘要通知 | 提示条 |
| `tool_error` | 服务端→App | 错误分类+建议 | 错误卡片 |
| `meta` | 服务端→App | 会话头部 model/token | 会话信息 |
| `tts_result` / `stt_result` / `tts_error` / `stt_error` | 双向 | 语音 | 播放/显示 |
| `done.usage` | 服务端→App | 真实 token 用量 | 状态行 |
| `tool_result.name` | 改造 | 真实工具名 | 工具名显示 |

### 6.2 App 状态行（先生扩展）
model / token 上传量（prompt）/ 下载量（completion）/ **缓存命中** / AI 输出 / 已压缩标记

### 6.3 新命令
- `model_switch`（切换当前模型——主机端配置的模型列表）
- `context_status`（查询当前上下文 token 状态）
- 语音：`voice_tts` / `voice_stt`（服务端代理时）→ 或走 HTTP REST

### 6.4 权限补全
- AndroidManifest 声明 RECORD_AUDIO 等（先生指出部分权限未声明无法系统授权）

---

## 七、验证方式

- 沙箱：语法 + 逻辑单测（mock 各错误场景——**先生偏好不使用假数据，但沙箱无 key：格式错误调试靠落盘文件验证**）
- 先生环境：make 重编译服务端 + App 0.2.0 实测
- 重点复测：并行工具调用 400 场景（B1）、Ollama 走 openai adapter、语音降级链

---

## 八、实施清单（一批次完——全部完成后编译提交）

- [x] 1. 统一 LLM 调用层（openai/anthropic adapter + provider.json + model_switch）
- [x] 2. 格式错误调试落盘（api_debug/，Key 脱敏）
- [x] 3. 错误分类 + 重试（429/5xx 退避）
- [x] 4. 上下文窗口动态获取
- [x] 5. 双记忆（importance 自决 + 自动注入限预算）
- [x] 6. 状态外部化延续（plan.md/notes.md 指针）
- [x] 7. 分层上下文 + 70% 预压缩 + context 事件
- [x] 8. 压缩保存 session_archives + 回溯读取
- [x] 9. 记忆索引/关键词/常量查找
- [x] 10. session_read/session_search 技能（默认允许）
- [x] 11. 工具错误 17 类 + tool_error 事件 + 知识库自动联动
- [x] 12. 并行结果 tool_call_id 回填
- [x] 13. tool_result 真实技能名
- [x] 14. 技能 schema 精简 + 分层注入
- [x] 15. system prompt 精简 + 静态缓存
- [x] 16. done.usage 真实提取 + token_usage 真落盘
- [x] 17. 语音系统全模块（TTS/STT/代理/降级链/HTTP REST/流式/连续对话/用量/清理）
- [x] 18. App：新事件适配 + 状态行 + 权限声明 + 语音 UI + 模型切换
- [x] 19. 协议 v2 已更新（本文件 + CHANGELOG 同步）
- [x] 20. 先生环境验证 → 编译提交 → 更新 CHANGELOG

---

## 九、协议与文档同步（已更新 2026-08-12）

- ✅ LING_OS_AI_PROTOCOL_v2.md：事件清单（10 新增）+ AI-AGENT#7/8/9 + 已知问题 11-15
- ✅ 本方案文档（定稿）
- ⏳ CHANGELOG.md：实施完成后更新 0.2.0 段
