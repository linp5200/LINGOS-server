# LING OS 系统类目架构（先生 2026-09-05 裁决定稿）

> 本文件 = 全系统"功能系统划分/命名/归属"的**唯一权威依据**。
> 原则：**一系统一职责**；类目名=功能词；命令前缀=系统名；重负载/硬件功能=独立进程；骨架=核心进程。

## 一、类目与系统

| 类目 | 系统名 | 命令前缀 | 职责 | 进程形态 |
|---|---|---|---|---|
| **核心** | 核心系统 core | core.* | 进程/事件总线/日志/运行监督(守护·启动编排)/命令路由/注册表 | lingos-core 主进程 |
| **通讯** | 通讯系统 comm | comm.* | TCP/WS/HTTP 认证/连接/路由 | 主进程内 |
| | 通知系统 notify | notify.* | 通知分发(本地/推送/铃声) | 主进程内 |
| | 语音系统 voice | voice.* | TTS/STT（人机语音通讯） | 独立服务(python) |
| **配置** | 配置系统 config | config.* | 配置读写/向导/校验/热更新/路径 | 主进程内 |
| **数据** | 数据系统 data | data.* | 存储/记忆/会话/知识/模型数据 | 主进程内+文件 |
| **插件** | 插件系统 plugin | plugin.* | 插件装载/热重载/进程插件 | 独立类 |
| **安全** | 安全系统 security | security.* | 权限/沙箱/令牌/审计 | 主进程内 |
| | 家居系统 home | home.* | 安防设备联动/HA | 独立桥 |
| | **监控系统 monitor** | monitor.* | 采集(V4L2/RTSP)/多路/预览/录像/拍照/IR/OSD/存储——**给人看** | **独立进程 lingos-monitor** |
| | 预警系统 alert | alert.* | 安全预警/分级/通知 | lingos-alertd |
| **AI** | AI 系统 ai | ai.* | 对话/推理编排 | ai_server(独立) |
| | 技能系统 skill | skill.* | 技能库/安装/执行 | ai 内 |
| | **AI 识别引擎 ai_vision** | ai_vision.* | 检测/标定/追踪——**给 AI 用**，消费监控帧 | **独立进程 lingos-ai-vision** |
| | 识别引擎 ocr | ocr.* | 文字识别（AI 内容） | 独立/python |
| **天气** | 天气系统 weather | weather.* | 天气数据/源切换 | python/模块 |

## 二、监控系统 与 AI 识别引擎 的边界（先生裁决——防职责冲突）

```
监控系统 monitor（给人看，同正常监控）
  └ 纯净实时画面：无检测框/置信度（默认）
       │ 帧(共享/sock)
AI 识别引擎 ai_vision（给 AI 用，非人看）
  └ 检测/标定/追踪 → 结果供 AI/规则/触发
       │ 叠加开关（高级设置→"AI 识别引擎叠加画面"）
监控画面 = 纯净帧 + (开关开时)识别结果渲染
```

- **采集/存储/预览/录像** → 监控系统（camera 职责并入 monitor）
- **检测/标定/追踪/OCR** → AI 识别引擎（原 vision 检测部 + ocr）
- 二者经**帧通道 + 事件**解耦，不互相内含

## 三、命名落地

- 源码目录：`src/<系统>/`（core/comm/config/data/plugin/security/home/monitor/ai/skill/ai_vision/ocr/weather/alert/notify/voice）
- 二进制：`lingos-<系统>`（lingos-monitor / lingos-ai-vision…）；核心保留 lingos_linux/lingosd 兼容
- 命令：`<系统>.<动作>` 前缀进统一 API + registry
- 配置：config 系统统一管（monitor.conf 由 config 读写——monitor 只管采集）

## 四、叠加态（用户可见逻辑）

- 默认监控实时画面 = 干净（AI 不打扰人）
- 高级设置 →「AI 识别引擎叠加画面」开：检测框/类别/置信度/坐标叠加监控画面
- 底层始终：monitor 帧 → ai_vision 分析；叠加只是"是否把结果画上监控画面"

---

## 五、脚本核对事实（2026-10-16 · 第11期自动清扫）

> 复跑：`python3 docs/i18n_consistency_scan.py`；明细见 `docs/双语与文档一致性报告.md`。
> **本节只记录事实，不改动第一~四节的设计权威结论**；带「待确认」的条目需类目负责人裁决后再动代码/文档。

- **命令前缀↔类目**：`src/shell/dotted_cmd.c` 现有 86 条 `系统.动作` 命令，覆盖前缀
  `core system comm voice config data plugin security home alert ai skill comm rules
  registry log memory health repair install snapshot remind/reminder control ui update`。
  - 文档第一节点名、但命令表中**暂无** `前缀.*` 命令的系统：`ai_vision` `monitor` `notify` `ocr` `weather`
    ——待确认：这些系统是否只经 registry/Python 面暴露（若是，本节补注即可；若应有命令面，则需补命令）。
  - 命令表存在、但第一节类目表未列的前缀：`control` `health` `install` `log` `memory` `registry`
    `remind`/`reminder` `repair` `rules` `snapshot` `system` `ui` `update`
    ——待确认：应归入现有类目（如 log/memory→data，system/update→core）还是新增类目。
- **二进制**：第一节提到的 `lingos-monitor` / `lingos-ai-vision` 等 0 缺失（Makefile 中均存在）。
- **双语（第11期清扫后）**：`tr(中文,中文)` 真缺失 **0** 处、未包 `tr()` 的用户可见中/英文句 **0** 处；
  本轮修掉 6 处"同串假双语"+ 1 处"拼中文再 tr(msg,msg)"+ 6 处单语提示（shell.c / weather.c /
  widget_terminal.c / widget_files.c）。`webui/` 为**设计单语**（无 i18n 机制），382 处中文仅登记不强制改。
- **文档覆盖**：86 条命令中 68 条未在本节之前的文档中被提及（明细见报告中 F 节）。
