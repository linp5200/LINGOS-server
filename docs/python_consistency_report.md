# Python 侧一致性校验报告（清单 ↔ 实际 handler）

生成方式（可复跑）：python3 tools/check_python_consistency.py

## 统计

| 项 | 数量 |
|---|---|
| 内置技能（register_skill） | 68 |
| 插件技能（@Skill/@Command） | 0 |
| 命令面（ai_server if cmd ==） | 113 |
| 扩展命令表（EXT_MAP） | 82 |
| 已解析 import | 548 |
| C 统一 API 命令 | 9 |
| 假数据/占位标记（信息项） | 10 |
| **ERROR** | **11** |
| WARN | 21 |
| INFO | 11 |

## 发现

| 级别 | 检查项 | 位置 | 说明 |
|---|---|---|---|
| ERROR | C | src/python/ai_server.py:5292 | 命令 ['voice_usage_query'] 分发的 cmd_voice_usage_query() 未定义 |
| ERROR | C | src/python/ai_server.py:5294 | 命令 ['audio_clear'] 分发的 cmd_audio_clear() 未定义 |
| ERROR | C | src/python/ai_server.py:5298 | 命令 ['ha_config_get'] 分发的 cmd_ha_config_get() 未定义 |
| ERROR | C | src/python/ai_server.py:5357 | 命令 ['ha_config_set'] 分发的 cmd_ha_config_set() 未定义 |
| ERROR | C | src/python/ai_server.py:5360 | 命令 ['ha_status'] 分发的 cmd_ha_status() 未定义 |
| ERROR | C | src/python/ai_server.py:5362 | 命令 ['ha_states'] 分发的 cmd_ha_states() 未定义 |
| ERROR | C | src/python/ai_server.py:5364 | 命令 ['ha_control'] 分发的 cmd_ha_control() 未定义 |
| ERROR | C | src/python/ai_server.py:5364 | 命令 ['ha_control'] 分发的 cmd_ha_control_confirm() 未定义 |
| ERROR | E | src/python/ha_integration.py:207 | 幽灵模块 websocket：既非本地文件，也不在已知依赖表（guarded） |
| ERROR | E | src/python/ocr_service.py:38 | 幽灵模块 pytesseract：既非本地文件，也不在已知依赖表（guarded） |
| ERROR | E | src/python/yolo_service.py:61 | 幽灵模块 ultralytics：既非本地文件，也不在已知依赖表（guarded） |
| WARN | E | src/python/ai_server.py:23 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/config_helpers.py:12 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/embed_service.py:17 | 第三方依赖 numpy 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/llm_unified.py:453 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/llm_unified.py:594 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/llm_unified.py:713 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/llm_unified.py:831 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/sub_ai_scheduler.py:18 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:207 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:222 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:236 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:256 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:277 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:298 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:317 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:399 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:415 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/voice_service.py:432 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/web_search.py:20 | 第三方依赖 requests 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/yolo_service.py:19 | 第三方依赖 numpy 未用 try/except 保护（运行期可能 ImportError） |
| WARN | E | src/python/yolo_service.py:21 | 第三方依赖 PIL 未用 try/except 保护（运行期可能 ImportError） |
| INFO | F | src/registry/function_registry_builtin.c:0 | C 统一 API 命令 9 条：api.describe, api.openapi, api.rely_status, config.get, config.set, debug.set_log_level, system.selfcheck, system.events, system.version |
| INFO | G | src/python/ai_server.py:595 | 标记：# 路由决策在 C 端完成，此处保留占位 |
| INFO | G | src/python/ai_server.py:604 | 标记：# 向量检索通过 syscall_client 调用，此处保留占位 |
| INFO | G | src/python/ai_server.py:1060 | 标记：# 【批次2】真实摘要生成（替代伪摘要占位；失败自动降级） |
| INFO | G | src/python/ai_server.py:1064 | 标记："""调用当前后端生成对话摘要；失败返回 None（触发占位降级）""" |
| INFO | G | src/python/ai_server.py:1120 | 标记：# 【批次2】真摘要：调用模型压缩旧消息（失败降级为占位文本） |
| INFO | G | src/python/home_ext.py:661 | 标记："""values: JSON {"scene_id":"abc"} —— 替换步骤中的 ${param} 占位符""" |
| INFO | G | src/python/skill_dev.py:39 | 标记："    # TODO: 实现技能逻辑\n" |
| INFO | G | src/python/skill_handlers.py:52 | 标记：# 降级：定义占位函数 |
| INFO | G | src/python/skill_loader.py:210 | 标记：# 替换命令中的占位符 {key} |
| INFO | G | src/python/sub_ai_worker.py:17 | 标记：引用方（真实接线，非占位）： |

## 检查项说明

- A 内置技能：register_skill(name, func) 的 func 必须在本模块定义/导入；重名/风险级别
- B 插件技能：plugin/*.py 的 @Skill/@Command 装饰器 name 与重名
- C 命令面：ai_server.py 的 if cmd == 分支必须调用已定义的 cmd_* 函数
- D 扩展命令表：ext_dispatch.EXT_MAP 的 (模块, 函数) 必须真实存在
- E 本地 import：裸模块导入必须命中本地文件 / 标准库 / 已知依赖表，否则为幽灵模块
- F C 统一 API：function_registry_builtin.c 注册清单（信息项，供跨语言比对）
- G 假数据标记：random. / TODO / 占位（信息项，人工判定，不自动删除）

