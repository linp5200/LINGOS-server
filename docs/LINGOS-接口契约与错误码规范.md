# LING OS 接口契约与错误码规范

> 版本：v1（2026-10-30）
> 状态：规范基线；未部署
> 作用：统一 HTTP / WebSocket / Unix Socket IPC / AI 工具 / 插件 / 配置 / 事件的契约，
>       保证跨版本兼容、可测试、可审计。

---

## 1. 原则

1. 契约优先（contract-first）：先定接口/错误码，再实现。
2. 版本化：对外接口带版本（/api/v1），破坏性变更升 v2。
3. 机器可读：错误必须带 code/message/retryable/trace_id。
4. 幂等：写操作支持幂等键（Idempotency-Key）。
5. 最小暴露：只返回必要字段；禁止泄露内部路径/密钥/堆栈。
6. 安全默认：鉴权、限流、审计默认开启；安全设置接口仅本地可写。
7. 可测试：每个接口有 schema + 契约用例。

---

## 2. 接口清单

| 接口 | 协议 | 方向 | 说明 |
|---|---|---|---|
| /api/v1/status | HTTP | App/WebUI → lingosd | 系统状态 |
| /api/v1/chat | HTTP/WS | App/WebUI → AI | 对话 |
| /api/v1/alert | HTTP/WS | App/WebUI → alertd | 告警查询/订阅 |
| /api/v1/options | HTTP | App/WebUI → config | 配置读取（写仅本地） |
| /api/v1/security | HTTP | 本地桌面 → 安全服务 | 安全设置（仅本地+再认证） |
| /api/v1/version | HTTP | 任意 → lingosd | 版本/edition/构建信息 |
| /ws | WebSocket | App/WebUI → lingosd | 事件/对话流 |
| lingosd.sock | Unix socket JSON | 进程间 | 核心 IPC |
| ai-tool:// | 本地协议 | AI → 工具宿主 | AI 工具调用（权限闸门） |
| plugin.sock | Unix socket | 插件 → 宿主 | 插件 RPC（沙箱） |
| 配置文件 | JSON | 系统 | schema 版本化 |
| 事件/告警 | JSON | 系统 → 订阅者 | 统一事件格式 |

---

## 3. 通用响应封装

成功：

    {
      "ok": true,
      "data": {},
      "request_id": "req_xxx",
      "trace_id": "tr_xxx",
      "version": "v1",
      "ts": "2026-10-30T12:00:00Z"
    }

失败：

    {
      "ok": false,
      "error": {
        "code": 40101,
        "message": "未授权",
        "details": {},
        "retryable": false,
        "trace_id": "tr_xxx"
      },
      "request_id": "req_xxx",
      "ts": "2026-10-30T12:00:00Z"
    }

规则：
- message 面向用户，可本地化；details 给开发者；
- 禁止把内部异常原文直接返回；
- 所有响应带 trace_id，日志可关联。

---

## 4. 错误码规范

格式：5 位数字 = 类别(1) + 子类(2) + 具体(2)；HTTP 状态映射见括号。

| 范围 | 类别 | HTTP 映射 |
|---|---|---|
| 0 | 成功 | 200 |
| 10000–19999 | 通用/参数 | 400/422 |
| 20000–29999 | 认证/授权 | 401/403 |
| 30000–39999 | 资源/状态冲突 | 404/409/410 |
| 40000–49999 | 限流/容量 | 429/503 |
| 50000–59999 | 外部依赖（模型/天气/网络） | 502/504 |
| 60000–69999 | AI/工具/插件 | 400/403/500 |
| 70000–79999 | 安全/隐私策略拒绝 | 403 |
| 80000–89999 | 更新/安装/启动 | 409/500 |
| 90000–99999 | 内部错误（不暴露细节） | 500 |

约定错误码示例：
- 10001 参数缺失；10002 参数非法；
- 20001 未认证；20002 token 过期；20003 权限不足；20004 需本地确认；
- 30001 资源不存在；30002 状态冲突；30003 版本不兼容；
- 40001 请求过频；40002 队列满；
- 50001 模型不可用；50002 网络超时；
- 60001 工具未授权；60002 工具范围越权；60003 插件签名无效；
- 70001 被隐私等级拒绝；70002 安全设置仅本地可改；
- 80001 安装失败；80002 启动失败；80003 更新校验失败；
- 90001 内部错误。

兼容规则：新增错误码只能追加，不得改变既有语义；删除需走弃用流程。

---

## 5. HTTP 规范

- 基础路径 /api/v1；
- 鉴权：Authorization: Bearer <token>（本地/局域网按策略）；安全设置接口额外要求本地会话凭据；
- 限流：429 + Retry-After；每 IP/每 token 限额；
- 缓存：GET 可 ETag；敏感数据 no-store；
- 超时：默认 30s；AI 流式用 WS；
- 幂等：POST 写操作接受 Idempotency-Key，重复请求返回同一结果；
- 分页：?limit=&cursor=，返回 next_cursor；
- 过滤/排序：白名单字段，禁止任意表达式；
- 版本：/api/v1；弃用响应头 Deprecation + Sunset。

---

## 6. WebSocket 规范

连接：/ws?token=...（或首帧认证）
帧格式：

    {"type":"hello","version":"v1","request_id":"..."}
    {"type":"subscribe","topics":["alert","chat","system"]}
    {"type":"event","topic":"alert","seq":123,"data":{},"ts":"..."}
    {"type":"ping"} / {"type":"pong"}
    {"type":"error","error":{"code":...}}

规则：
- 心跳 20–30s；断线重连；seq 支持断点续传；
- 单连接订阅上限；背压（队列满发 error/断开）；
- 认证失败关闭 1008；限流断开 1013。

---

## 7. Unix Socket IPC 规范

- 传输：AF_UNIX SOCK_STREAM；
- 编码：newline-delimited JSON（一行一消息）；
- 消息最大 1MB；超限拒绝；
- 请求/响应带 id；支持通知（无 id）；
- 权限：socket 0600，仅允许指定用户/组；
- 版本握手：首帧 hello 带协议版本，不兼容拒绝。

---

## 8. AI 工具契约

manifest 字段：

    {
      "id": "tool.weather.get",
      "name": "天气查询",
      "version": "1.0.0",
      "capabilities": ["network"],
      "data_scope": ["城市级位置"],
      "risk": "low|medium|high",
      "confirmation": "auto|once|always|local_typed",
      "rate_limit": "10/min",
      "timeout_ms": 10000,
      "audit": true
    }

调用请求：

    {
      "tool_id": "tool.weather.get",
      "purpose": "回答用户天气问题",
      "scope": {"city": "北京"},
      "args": {},
      "session_id": "sess_xxx",
      "trace_id": "tr_xxx"
    }

规则：
- 每次调用必须带 purpose + scope；缺失直接拒绝（70001）；
- 权限按 L0–L4 策略校验；L3/L4 需本地确认；
- 返回结果不含敏感原文；审计记录 tool_id/purpose/scope/结果摘要；
- 超时/取消信号必须支持；工具不可修改安全设置。

---

## 9. 插件契约

manifest：id/name/version/signature/capabilities/permissions/entry/compat。
生命周期：install → verify signature → sandbox spawn → handshake → serve → stop → uninstall。
限制：
- 默认无网络/文件/设备权限；按 manifest + 用户授权；
- 只能通过 plugin.sock RPC 访问宿主能力（broker 校验）；
- 资源限额（CPU/内存/进程数）；超限 kill；
- 禁止访问安全设置、密钥、其他插件数据；
- 版本兼容：宿主 API 版本 + min/max。

---

## 10. 配置契约

- 文件：/etc/lingos/config.json（系统）、用户配置独立；
- 顶层 schema_version；
- 变更必须提供迁移脚本 + 备份 + 回滚；
- 敏感字段加密（api_key_enc 等）；
- 配置读取 API 脱敏；写入仅本地特权通道；
- 校验失败启动时回退默认 + 明确告警。

---

## 11. 事件/告警契约

事件字段：id、type、severity、source、ts、trace_id、data、privacy_class。
告警：severity（info/warning/critical/life_safety）、不可静默降级规则、重试与确认状态。
订阅：WebSocket topic；本地 Unix socket 通知；Webhook（可选，需鉴权与签名）。

---

## 12. 契约测试与兼容矩阵

- CI 中对每个接口跑 schema 校验 + 典型/边界/错误用例；
- 维护兼容矩阵：v1 客户端 vs v1/v2 服务端；
- 破坏性变更必须升版本，并提供迁移指南；
- 弃用：Deprecation 头 + 文档 + 至少一个 MINOR 周期。

---

## 13. 安全与隐私约束（接口层）

- 安全设置写入仅本地控制台（远程 403 + 审计，错误码 70002）；
- 位置/生物特征/密钥字段永不返回明文；
- 所有接口默认限流；敏感接口二次认证；
- 日志脱敏：token/密钥/位置精确值不落日志；
- 接口文档标注隐私等级与数据字段。
