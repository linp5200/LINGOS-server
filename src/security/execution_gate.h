/**
 * @file    execution_gate.h
 * @brief   执行门模块：统一输入安全审查
 * @version LN-B-5.0.0.0
 */

#ifndef EXECUTION_GATE_H
#define EXECUTION_GATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 输入来源类型
 * ============================================================ */

typedef enum {
    GATE_SOURCE_SHELL = 0,
    GATE_SOURCE_AI_PROMPT,
    GATE_SOURCE_AI_TOOL,
    GATE_SOURCE_CONFIG_WIZARD,
    GATE_SOURCE_TUI_TERMINAL,
    GATE_SOURCE_RULE_ENGINE,
    GATE_SOURCE_HOST_CMD,
    GATE_SOURCE_API_REQUEST,
    GATE_SOURCE_MQTT,
    GATE_SOURCE_FILE,
    GATE_SOURCE_PLUGIN
} gate_source_t;

/* ============================================================
 * 审查结果
 * ============================================================ */

typedef enum {
    GATE_RESULT_ALLOW = 0,
    GATE_RESULT_DENY,
    GATE_RESULT_NEED_CONFIRM
} gate_result_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * @brief 检查输入是否允许通过
 * @param source 输入来源
 * @param input 输入内容
 * @param context 上下文信息（如命令名）
 * @param reason 输出拒绝原因
 * @param reason_len 缓冲区大小
 * @return 审查结果
 */
gate_result_t execution_gate_check(gate_source_t source,
                                   const char *input,
                                   const char *context,
                                   char *reason,
                                   size_t reason_len);

/**
 * @brief 设置执行门模式
 * @param mode "strict" | "balanced" | "permissive"
 * @return 0 成功，-1 失败
 */
int execution_gate_set_mode(const char *mode);

/**
 * @brief 获取当前模式
 * @return 模式字符串
 */
const char* execution_gate_get_mode(void);

/**
 * @brief 添加白名单模式
 * @param pattern 白名单模式（支持通配符）
 * @return 0 成功，-1 失败
 */
int execution_gate_whitelist_add(const char *pattern);

/**
 * @brief 移除白名单模式
 * @param pattern 白名单模式
 * @return 0 成功，-1 失败
 */
int execution_gate_whitelist_remove(const char *pattern);

/**
 * @brief 列出白名单
 * @param out 输出缓冲区
 * @param out_len 缓冲区大小
 * @return 0 成功，-1 失败
 */
int execution_gate_whitelist_list(char *out, size_t out_len);

/**
 * @brief 获取输入来源名称
 * @param source 来源枚举
 * @return 名称字符串
 */
const char* execution_gate_source_name(gate_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* EXECUTION_GATE_H */