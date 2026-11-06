# LING OS AI Agent 架构（DSH 风格）

> 版本：v1（2026-10-30）
> 目标：把 AI 从“聊天框”升级为受权限约束的管家/助手：能计划、调工具、可审计、可中断。

## 1. 分层

    用户/语音/桌面
        │
    Agent Runtime（计划 -> 工具 -> 观察 -> 反思）
        │
    权限闸门（L0-L4 / purpose+scope / 用户确认）
        │
    工具注册表（manifest：capabilities/risk/data_scope/confirmation）
        │
    插件沙箱（签名 + bwrap + broker）
        │
    审计（哈希链）/ 记忆（加密）/ 模型（本地优先）

## 2. Agent 循环（DSH 借鉴点）

1. 理解目标，生成计划（可多步）；
2. 每一步选择工具，声明 purpose + scope；
3. 权限闸门：按隐私等级决定 自动/需确认/拒绝；
4. 执行工具，记录观察结果；
5. 反思：失败/拒绝时调整计划，必要时请求用户确认；
6. 全程 trace_id + 审计；可中断、可回滚副作用。

## 3. 已落地骨架

- src/python/agent_runtime.py：ToolSpec/ToolCall/ToolResult、PermissionGate、AgentRuntime；
- distro/images/live/lingos-ai-tool-gate：命令行权限闸门（L0-L4）；
- distro/security/ai-tool-manifest-schema.json：工具 manifest 规范；
- distro/security/ai-tools/*.json：示例工具（天气）；
- tests/test_agent_runtime.py：7 个单测（闸门/执行/拒绝/确认）。

## 4. 权限模型

| 等级 | 自动允许 | 必须确认 | 禁止 |
|---|---|---|---|
| L0 | read/compute | write/network/exec/device | - |
| L1 | read/compute/network | write/exec/device | - |
| L2 | read/compute | write/network/exec/device | - |
| L3 | 无 | 全部 | 插件/后台代理 |
| L4 | 无 | 全部（且需 scope+时限） | 插件/后台/自动批准 |

硬规则：AI 不得修改安全设置、不得读密钥/生物特征、工具结果入记忆前脱敏。

## 5. 工具 manifest 示例

    {
      "id": "tool.weather.get",
      "capabilities": ["network"],
      "data_scope": ["城市级位置"],
      "risk": "low",
      "confirmation": "auto",
      "rate_limit": "10/min",
      "timeout_ms": 10000
    }

## 6. 插件沙箱

- 仅签名插件；manifest 校验（lingos-plugin-gate）；
- bwrap 运行：无网络/无 home 默认，按 permissions 授权；
- 宿主 broker 校验每次 RPC；资源限额；超限 kill；
- L2+ 默认禁用，L4 禁止；审计所有安装/调用。

## 7. 后续（0.11+）

- 声纹身份 → 用户档案；
- 事件/物体记录 → 记忆检索工具；
- 指向性识别 → 场景工具；
- 本地模型优先、云端显式授权；
- 计划可视化 / 中断 / 回滚；
- 评测集（任务成功率、拒绝正确率、越权=0）。
