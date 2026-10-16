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
| 假数据/占位标记（信息项） | 6 |
| **ERROR** | **0** |
| WARN | 7 |
| INFO | 21 |

## 发现

| 级别 | 检查项 | 位置 | 说明 |
|---|---|---|---|
| WARN | E | src/python/ai_server.py:23 | 第三方依赖 requests 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/config_helpers.py:12 | 第三方依赖 requests 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/embed_service.py:17 | 第三方依赖 numpy 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/sub_ai_scheduler.py:18 | 第三方依赖 requests 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/web_search.py:20 | 第三方依赖 requests 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/yolo_service.py:19 | 第三方依赖 numpy 未用 try/except 保护（模块级 import） |
| WARN | E | src/python/yolo_service.py:21 | 第三方依赖 PIL 未用 try/except 保护（模块级 import） |
| INFO | E | src/python/llm_unified.py:453 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/llm_unified.py:594 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/llm_unified.py:713 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/llm_unified.py:831 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:207 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:222 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:236 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:256 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:277 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:298 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:317 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:399 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:415 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | E | src/python/voice_service.py:432 | 第三方依赖 requests 未用 try/except 保护（函数内 import，仅该功能受影响） |
| INFO | F | src/registry/function_registry_builtin.c:0 | C 统一 API 命令 9 条：api.describe, api.openapi, api.rely_status, config.get, config.set, debug.set_log_level, system.selfcheck, system.events, system.version |
| INFO | G | src/python/ai_server.py:1049 | 标记：# 【批次2】真实摘要生成（替代伪摘要占位；失败自动降级） |
| INFO | G | src/python/home_ext.py:661 | 标记："""values: JSON {"scene_id":"abc"} —— 替换步骤中的 ${param} 占位符""" |
| INFO | G | src/python/skill_dev.py:39 | 标记："    # TODO: 实现技能逻辑\n" |
| INFO | G | src/python/skill_handlers.py:52 | 标记：# 降级：定义占位函数 |
| INFO | G | src/python/skill_loader.py:210 | 标记：# 替换命令中的占位符 {key} |
| INFO | G | src/python/sub_ai_worker.py:17 | 标记：引用方（真实接线，非占位）： |

## 本轮修复（task-10）

- ① 预警级别桶补齐 info/L0-L5：原 ai_server.cmd_alert_summary 只有 L0~L3+info，C 端 L4/L5（生命线）被统计成 info → App 显示成提示级。新增 src/python/alert_store.py 统一级别归一化/统计（含数值型与 level_num），并新增 highest_level。
- ① alert_query 技能幽灵引用修复：原 from alert_manager import ... （alert_manager 只有 C 实现、无 Python 绑定）→ 技能恒失败。改读 /LINGOS/data/alerts（与命令面 cmd_alert_query 同源）。
- ② 删除恒 True 且全仓库零读取点的死标志 _model_router_available / _vector_search_available（ai_server.py）。
- ② 对话摘要失败时不再编造“已摘要 N 条”的假摘要（原文丢失）：改为保留原消息硬截断（信息保真，仍省 token）。

## 检查项说明

- A 内置技能：register_skill(name, func) 的 func 必须在本模块定义/导入；重名/风险级别
- B 插件技能：plugin/*.py 的 @Skill/@Command 装饰器 name 与重名
- C 命令面：ai_server.py 的 if cmd == 分支必须调用已定义的 cmd_* 函数
- D 扩展命令表：ext_dispatch.EXT_MAP 的 (模块, 函数) 必须真实存在
- E 本地 import：裸模块导入必须命中本地文件 / 标准库 / 已知依赖表，否则为幽灵模块
- F C 统一 API：function_registry_builtin.c 注册清单（信息项，供跨语言比对）
- G 假数据标记：random. / TODO / 占位（信息项，人工判定，不自动删除）

