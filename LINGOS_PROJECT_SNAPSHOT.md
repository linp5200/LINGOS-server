# LING OS 项目状态快照（2026-08-13——App 闪退换对话保全）

> 用途：先生 App 闪退（单条消息渲染问题）——换对话继续。此文件 + 记忆系统（2026-08-13.md 多条）可恢复全部上下文。

---

## 一、当前版本状态

| 端 | 版本 | 状态 |
|---|---|---|
| 服务端 Python | 0.2.0 | ✅ 已实施（统一LLM层/语音/上下文引擎/17类错误） |
| App | 0.2.0+11 | ✅ CI 成功 Release 已发（4 包）；**0.2.1 问题仅记录未部署** |
| C 端 | 未动 | 本次无 C 改动（websocket_server 沿用） |

## 二、服务端 0.2.0 已实施内容（全部验证通过）

- **llm_unified.py**（新）：openai/anthropic 原生直连不转换、400 自动回退（去 reasoning_effort 重发）、真实 usage 提取（含缓存命中）、api_debug 落盘（Key 脱敏）、429/5xx 重试、上下文窗口动态（配置→内置表→/v1/models→无限）
- **voice_service.py**（新）：8 家提供商直连、降级链 espeak-ng→piper→设备本地、HTTP 8088、voice_usage.jsonl、24h 清理
- **ai_server.py**：4 个 call 函数→统一层包装（签名兼容）、上下文引擎（70% 预压缩+context 事件+session_archives 落盘+任务指针）、17 类工具错误+tool_error 事件+真实技能名、schema A+B 分层注入（47 核心全量+其余精简）、提示词去重、9 新命令（provider_list/model_switch/provider_add/remove/context_status/voice_tts/stt/voice_usage_query/audio_clear）、HTTP 8088（POST tts/stt + GET file——Bearer token+目录穿越防护）
- **skill_handlers.py**：memory_write 加 importance（[重要] 前缀双记忆）
- **memory_retrieval.py**：important_only 过滤 + search_keywords/find_important/search_constants 索引增强
- **App 0.2.0**：新事件（context/toolError/meta/tts_*/stt_*）、状态行（model+token↑↓+缓存+输出+压缩）、错误卡片、模型切换 UI、语音 UI（record 6.1.1 录音→STT + audioplayers 6.1.0 朗读——**已降级兼容低版本 Flutter**）、manifest 补 FOREGROUND_SERVICE 等

## 三、0.2.1 问题清单（仅记录未部署——13 项）

1. 离线模式（缓存+断连只读+操作拦截）
2. 会话页模型切换缺失
3. 工具调用后 AI 对话中断（须用户再发）
4. 工具块/思考块展开收缩
5. 外观"流式思考展开思考链"
6. 会话内模型切换（会话名+换模型+缓存重置提示）
7. 开局显示三选项（上次会话/会话列表/仪表盘——默认会话列表）
8. 权限授予优化（覆盖全/声明全/失败跳系统设置）
9. 会话管理：占用显示（服务端 session_list 加 token）/创建完善/模型不同步修复（requestJson 冲突——按 cmd 匹配）
10. 语音降级链 Bug（未配置不到设备本地 TTS/STT）
11. HA 集成（AI-AGENT#10 定稿）
12. espeak-ng/piper 装进安装系统
13. 依赖清单文档化（install_deps.sh + DEPENDENCIES.md）

## 四、HA 集成定稿（AI-AGENT#10——0.2.1 实施）

连接 Y（A MQTT 上报复用 C 端 + B REST 控制 + C WS 实时事件订阅）；功能全要；权限 ha_control 默认直接执行高风险确认；App 改名 B1（HA→Help AI 仅显示名）；配置入口 C2（设置主页独立项，不属 AI 配置）；C 端增强（订阅命令主题，需 make 重编译）；语音联动可串。

## 五、先生环境部署待办（0.2.0）

```bash
cp src/python/{ai_server,llm_unified,voice_service,skill_handlers,memory_retrieval}.py /LINGOS/bin/
chmod +x /LINGOS/bin/*.py
# 重启 AI server → 首次启动自动生成 provider.json（legacy 兜底）
# 装 App 0.2.0 → 复测并行工具 400 场景
```

## 六、关键架构事实

- **AI 调用均在主机端**（先生环境跑 ai_server.py）——App 纯前端消费流式事件
- 现有两套 HA：Help AI Data（ha_write/ha_search AI 帮助档案）≠ Home Assistant（智能家居集成）
- 沙箱=Alpine/musl，先生环境=proot ubuntu/glibc——**沙箱二进制不能在先生环境跑**，先生环境原生 make 编译
- 语音音频不走 WS：WS 信令 + HTTP 8088 REST（先生裁决）
- App 构建以 CI 为准（先生本地无构建痕迹）；CI 桌面端 continue-on-error 不阻塞 Android

## 七、文档与仓库位置

| 文档 | 位置 |
|---|---|
| 协议 v2（AI-AGENT#1~10） | /var/minis/mounts/LINGOS/00_协议核心/LING_OS_AI_PROTOCOL_v2.md |
| 0.2.0 方案 | LINGOS_APP_0.2.0_PLAN.md（workspace/先生目录/主仓） |
| 0.2.1 清单 | LINGOS_APP_0.2.1_ISSUES.md（workspace/先生目录） |
| 部署说明 | LINGOS_0.2.0_DEPLOY.md |
| CHANGELOG | workspace/lingos-app/CHANGELOG.md |
| App 仓库 | /var/minis/workspace/lingos-app（GitHub: linp5200/LINGOS-APP） |
| 服务端源码 | /var/minis/mounts/LINGOS_Aach64_termux_Proot_Linux/src/python/ |

## 八、最近讨论状态

- HA 集成：✅ 讨论定稿（连接 Y 最后裁决）→ 待 0.2.1 实施
- 依赖打包：讨论中（方案 1+2 结合倾向，install_deps.sh）——先生最后在问 C 库编译期/运行期问题（已实测回答：动态链接运行期需要 .so 本体）
- 服务端默认部署内容：已梳理（main() 自动建目录 + ensure_* 生成默认配置 + provider.json legacy 兜底）
- 语音相关部署：espeak-ng/piper 应装进安装系统（先生决策）——未实施
