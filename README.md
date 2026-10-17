# LING OS · AI 原生 Linux 设备系统

> **OS 为体 · Agent 为用** —— 一套跑在 PC / 迷你主机上的 AI 原生 Linux：
> 底面是标准 Linux（Wayland 桌面、系统服务、包管理、OTA），上面是常驻的系统级 AI 助理（诺玛式），
> 再叠加智能家居中枢与全端客户端；外部内容通过插件接入，被解析、记忆、检索、行动。

当前版本：**0.9.0（内测开发中）** · 主仓库（服务端 + WebUI + 系统层） · App 见 [LINGOS-APP](https://github.com/linp5200/LINGOS-APP)

---

## 一、这是什么

- **设备系统层（C）**：lingosd 主程序、supervisor、alertd、visiond、voiced、monitor/ai_vision、安装/更新、应用沙箱（seccomp + namespace）、驱动与设备管理。
- **Agent 层（Python）**：模型统一层（多 Provider）、68 个 AI 技能、82 个扩展命令、MCP 客户端、会话/记忆 L1-L3、上下文压缩、子 AI 调度、危机模式。
- **交互层**：WebUI（25 页真实数据，将作为桌面 kiosk 壳）、Flutter App、语音 TTS（STT 规划中）。
- **接口面**：统一命令协议（HTTP /api/cmd、WebSocket 2939）、OpenAPI（api.openapi）、lingosctl CLI、MQTT/HA 集成、webhook。

目标形态：**电脑上的 AI 手机 + 智能家居中枢 + 常驻系统助理**（人格参考"诺玛 / 红后"）。

## 二、当前状态（诚实标注）

- ✅ 可用：命令行/协议面（251 条可达命令：112 核心 + 82 扩展 + 68 技能）、事件总线、安全策略、预警本地链、MQTT/HA 集成、rootfs 安装、增量更新、技能系统、记忆、双语。
- 🟡 开发中：图形桌面（Wayland 资产已就绪：labwc/waybar/wofi/kiosk）、外部内容解析插件（管线+5 内置插件已实现）、OTA A/B 与真回滚、可观测/评测、契约测试。
- 🟠 待收口：App/WebUI 部分功能受命令契约问题影响、个别技能为回退实现。
- ❌ 未开始：镜像构建、包管理、备份恢复、真机长稳（J1–J8）。

## 三、架构

    ┌────────────────────────────────────────────────────────────┐
    │ 交互层   WebUI（桌面 kiosk / 浏览器） · Flutter App · 语音      │
    ├────────────────────────────────────────────────────────────┤
    │ 接口层   HTTP 8080 (/api/cmd /system/health) · WS 2939       │
    │          OpenAPI · lingosctl · MQTT/HA · webhook             │
    ├────────────────────────────────────────────────────────────┤
    │ Agent 层 Python ai_server：模型路由/技能/扩展/MCP/记忆/会话/压缩  │
    ├────────────────────────────────────────────────────────────┤
    │ 系统层   C：lingosd · supervisor · alertd · visiond · voiced  │
    │          monitor/ai_vision · install/update · app_sandbox    │
    ├────────────────────────────────────────────────────────────┤
    │ Linux 底座：内核/BSP · Wayland 桌面 · 网络/音频 · systemd 兼容  │
    └────────────────────────────────────────────────────────────┘

## 四、构建与运行

依赖：Linux（aarch64/x86_64）、gcc、make、python3、libcurl 等（见 DEPENDENCIES.md）。

    make -j                    # 构建主程序与各守护进程
    sudo bash scripts/lingos.sh start      # 启动
    bash scripts/bundle.sh     # 打全量包（CI 使用同一脚本）

开发纪律（当前阶段）：本地不编译，只做静态审核：

    gcc -fsyntax-only -Wall -Wextra <files>   # C 零警告
    python3 -m py_compile <files>             # Python

CI：GitHub Actions（build-bundle，双架构全量包 + Release 资产）。

## 五、协议与接口

| 接口 | 用途 |
|---|---|
| POST /api/cmd | 统一命令代理（HTTP 8080） |
| WS 2939 | App/桌面实时通道：chat 流事件、command、auth |
| GET /system/health | 主机存活/版本 |
| api.describe / api.openapi / api.rely_status | 命令清单与 OpenAPI 描述 |
| lingosctl | CLI：api.describe / config.get / system.selfcheck 等 |
| MQTT / HA / webhook | 智能家居与外部系统集成 |
| ingest_add/status/list/search/plugins | 外部内容接入插件管线（S16）：URL/文件/文本→解析→切块→入库→检索 |
| remote_status / vnc_start / vnc_stop | 远程访问状态与 VNC 控制（wayvnc） |

## 六、目录结构

    src/        C 系统层（daemon/api/alert/vision/voice/ai/security/registry/...）
    python/     Agent 层（ai_server/llm_unified/skills/ext/memory/mcp/...）
    webui/      WebUI（25 页真实数据）+ 三主题（A/B/C）+ 主题预览
    distro/     发行版资产：desktop（labwc/waybar/wofi/kiosk/wayvnc）
    frontend/   Qt6 桌面前端（冻结，可选）
    docs/       架构/插件/i18n 文档
    scripts/    构建/打包/部署
    tests/      零散测试（契约测试规划中）

## 七、文档

- [CHANGELOG-server.md](CHANGELOG-server.md)：版本与变更记录
- [DEPENDENCIES.md](DEPENDENCIES.md)：依赖清单
- [docs/ARCHITECTURE_系统类目.md](docs/ARCHITECTURE_系统类目.md)：系统类目架构
- [docs/PLUGIN_ARCHITECTURE.md](docs/PLUGIN_ARCHITECTURE.md)：插件化架构（含外部内容解析扩展设计基线）
- [docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md)：HTTP / WebSocket / VNC 远程访问矩阵
- webui/preview/theme-preview.html：三种 UI 方向预览（A 静默深色 / B 暖白家居 / C 诺玛 HUD）

## 八、路线图

1. 收口（进行中）：命令契约修复、聊天渲染、会话历史、天气/预警、空态与死按钮。
2. 桌面：Wayland（labwc + waybar + WebUI kiosk）替代 TUI；Qt 前端冻结。
3. 插件：外部内容输入与解析（网页/PDF/RSS/音视频/OCR）→ 记忆/RAG。
4. OS 对齐：RAUC A/B 更新与回滚、OCI/.desktop 应用模型、NetworkManager/PipeWire、镜像构建。
5. Agent 基础设施：MCP server、OpenAI 兼容网关、AgentEngine 可替换、可观测/评测。

## 九、安全与隐私

- 本地优先：对话、记忆、配置默认存本机；模型 Provider 可自选（本地/云端）。
- 权限闸门：技能执行前统一权限校验；高风险操作需审批；应用沙箱（seccomp + namespace）。
- 预警=生命线：高危预警强制醒目、不可静默降级、失败如实上报（不报"平安"）。
- 第三方插件默认关闭、沙箱运行、权限最小化。

## 十、许可

当前为内测公开仓库，未附开源许可证（保留所有权利）；如需开源协议授权请与作者联系。

---

> LING OS 正在从"功能铺开"转入"端到端收口"阶段：先把首启/对话/控家/预警四条黄金路径做到 100% 可用，再扩展。欢迎 issue 反馈。
