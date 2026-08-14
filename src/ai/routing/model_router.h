/**
 * @file    model_router.h
 * @brief   多模型路由 - 根据任务类型自动选择最优模型
 * @version LN-B-4.2.0.0
 */

#ifndef AI_ROUTING_MODEL_ROUTER_H
#define AI_ROUTING_MODEL_ROUTER_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 意图类型枚举
 * ============================================================ */

typedef enum {
    INTENT_CHAT = 0,        /* 闲聊/问候 - 快速响应 */
    INTENT_REASONING,       /* 复杂推理 - 需要强模型 */
    INTENT_CODE,            /* 代码生成/分析 - 需要强模型 */
    INTENT_SIMPLE_QA,       /* 简单问答 - 轻量模型 */
    INTENT_SYSTEM,          /* 系统命令 - 快速响应 */
    INTENT_UNKNOWN          /* 未知意图 - 使用默认模型 */
} intent_type_t;

/* ============================================================
 * 模型类型枚举
 * ============================================================ */

typedef enum {
    MODEL_OLLAMA = 0,       /* 本地 Ollama 模型 (快速) */
    MODEL_DEEPSEEK,         /* DeepSeek 云端模型 (强推理) */
    MODEL_PLUGIN,           /* 插件模型 (自定义) */
    MODEL_DEFAULT           /* 系统默认 */
} model_type_t;

/* ============================================================
 * 路由规则结构
 * ============================================================ */

typedef struct {
    intent_type_t intent;          /* 意图类型 */
    model_type_t model;            /* 对应模型 */
    char variant[16];              /* 模型变体: "pro" / "flash" */
    int thinking_enabled;          /* 是否启用思考模式 (1/0) */
    char keywords[10][32];         /* 关键词列表 (最多10个) */
    int keyword_count;             /* 关键词数量 */
    int priority;                  /* 优先级 (数字越大优先级越高) */
} routing_rule_t;

/* ============================================================
 * 路由结果结构
 * ============================================================ */

typedef struct {
    intent_type_t intent;          /* 识别的意图 */
    model_type_t model;            /* 推荐的模型 */
    char variant[16];              /* 模型变体: "pro" / "flash" */
    int thinking_enabled;          /* 是否启用思考模式 (1/0) */
    float confidence;              /* 置信度 (0-1) */
    char reason[128];              /* 决策原因 */
} routing_result_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

int model_router_init(void);
int model_router_route(const char *message, const char *session_id, routing_result_t *result);
const char* model_router_intent_name(intent_type_t intent);
const char* model_router_model_name(model_type_t model);
int model_router_rule_count(void);
int model_router_add_rule(const routing_rule_t *rule);
int model_router_load_config(const char *path);
int model_router_save_config(const char *path);
void model_router_cleanup(void);

#endif /* AI_ROUTING_MODEL_ROUTER_H */