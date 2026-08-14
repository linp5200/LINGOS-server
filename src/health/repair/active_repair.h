/**
 * @file    active_repair.h
 * @brief   主动健康修复（自愈）接口
 * @version LN-B-4.2.0.0
 */

#ifndef HEALTH_REPAIR_ACTIVE_REPAIR_H
#define HEALTH_REPAIR_ACTIVE_REPAIR_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 修复动作类型枚举
 * ============================================================ */

typedef enum {
    ACTION_UNKNOWN = 0,
    ACTION_CLEAN_CACHE,        /* 清理缓存 */
    ACTION_CLEAN_LOGS,         /* 清理日志 */
    ACTION_RESTART_AI_SERVER,  /* 重启 AI 服务器 */
    ACTION_RESTART_DAEMON,     /* 重启守护进程 */
    ACTION_RESTART_SERVICE,    /* 重启指定服务 */
    ACTION_NOTIFY_USER,        /* 通知用户 */
    ACTION_ROLLBACK,           /* 回滚系统 */
    ACTION_REPAIR_CONFIG,      /* 修复配置 */
    ACTION_REPAIR_PACK         /* 生成修复包 */
} repair_action_type_t;

/* ============================================================
 * 修复动作结构
 * ============================================================ */

typedef struct {
    repair_action_type_t type;   /* 动作类型 */
    char service_name[64];       /* 服务名称（ACTION_RESTART_SERVICE 时使用） */
    int priority;                /* 优先级（数字越小越先执行） */
    int timeout_sec;             /* 超时秒数 */
} repair_action_t;

/* ============================================================
 * 修复策略结构
 * ============================================================ */

typedef struct {
    char error_pattern[128];     /* 错误匹配模式 */
    int severity;                /* 严重程度 (1-5, 5 最高) */
    repair_action_t actions[4];  /* 修复动作列表 */
    int action_count;            /* 动作数量 */
    char fallback[64];           /* 降级策略名称 */
} repair_strategy_t;

/* ============================================================
 * 修复结果结构
 * ============================================================ */

typedef struct {
    int success;                 /* 是否成功 (1/0) */
    char action_used[64];        /* 使用的动作名称 */
    char error_msg[256];         /* 错误信息 */
    int64_t duration_ms;         /* 执行耗时（毫秒） */
} repair_result_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化主动修复系统
 * @return 0 成功，-1 失败
 */
int active_repair_init(void);

/**
 * @brief 触发修复流程
 * @param error_msg 错误消息
 * @param error_source 错误来源（如 "health_watchdog", "skill_executor" 等）
 * @param result 输出修复结果
 * @return 0 成功，-1 失败
 */
int active_repair_trigger(const char *error_msg, const char *error_source, repair_result_t *result);

/**
 * @brief 加载修复策略配置
 * @param path 配置文件路径（NULL 使用默认路径）
 * @return 0 成功，-1 失败
 */
int active_repair_load_strategies(const char *path);

/**
 * @brief 获取策略数量
 * @return 策略数量
 */
int active_repair_strategy_count(void);

/**
 * @brief 获取修复历史
 * @param out 输出缓冲区
 * @param out_len 缓冲区大小
 * @param limit 最大条目数
 * @return 实际条目数
 */
int active_repair_get_history(char *out, size_t out_len, int limit);

/**
 * @brief 清理修复系统
 */
void active_repair_cleanup(void);

/**
 * @brief 获取动作类型名称
 * @param type 动作类型枚举
 * @return 名称字符串
 */
const char* active_repair_action_name(repair_action_type_t type);

#endif /* HEALTH_REPAIR_ACTIVE_REPAIR_H */