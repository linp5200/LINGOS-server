/**
 * @file    ai_reminder.h
 * @brief   AI 主动任务提醒接口
 * @version LN-B-4.2.0.0
 */

#ifndef AI_REMINDER_AI_REMINDER_H
#define AI_REMINDER_AI_REMINDER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ============================================================
 * 提醒状态枚举
 * ============================================================ */

typedef enum {
    REMINDER_STATUS_PENDING = 0,    /* 待触发 */
    REMINDER_STATUS_TRIGGERED,      /* 已触发 */
    REMINDER_STATUS_CANCELLED,      /* 已取消 */
    REMINDER_STATUS_EXPIRED         /* 已过期 */
} reminder_status_t;

/* ============================================================
 * 提醒结构
 * ============================================================ */

typedef struct {
    char id[64];                    /* 提醒 ID (reminder_YYYYMMDD_HHMMSS_XXX) */
    char content[512];              /* 提醒内容 */
    time_t trigger_time;            /* 触发时间 */
    time_t created_at;              /* 创建时间 */
    time_t triggered_at;            /* 实际触发时间 */
    reminder_status_t status;       /* 状态 */
    int repeat;                     /* 重复次数 (0 = 不重复) */
    int repeat_interval;            /* 重复间隔（秒） */
    char session_id[64];            /* 关联会话 ID */
} reminder_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化提醒系统
 * @return 0 成功，-1 失败
 */
int reminder_init(void);

/**
 * @brief 添加提醒
 * @param content 提醒内容
 * @param trigger_time 触发时间戳
 * @param repeat 重复次数
 * @param repeat_interval 重复间隔（秒）
 * @param session_id 关联会话 ID（可选）
 * @param out_id 输出提醒 ID
 * @param out_id_len 输出缓冲区大小
 * @return 0 成功，-1 失败
 */
int reminder_add(const char *content, time_t trigger_time,
                 int repeat, int repeat_interval, const char *session_id,
                 char *out_id, size_t out_id_len);

/**
 * @brief 删除提醒
 * @param id 提醒 ID
 * @return 0 成功，-1 失败
 */
int reminder_delete(const char *id);

/**
 * @brief 获取提醒列表
 * @param out 输出数组
 * @param max_count 最大数量
 * @param include_triggered 是否包含已触发的
 * @return 实际数量
 */
int reminder_list(reminder_t *out, int max_count, int include_triggered);

/**
 * @brief 获取单个提醒
 * @param id 提醒 ID
 * @param out 输出结构
 * @return 0 成功，-1 失败
 */
int reminder_get(const char *id, reminder_t *out);

/**
 * @brief 触发提醒（由调度器调用）
 * @param id 提醒 ID
 * @return 0 成功，-1 失败
 */
int reminder_trigger(const char *id);

/**
 * @brief 获取提醒数量
 * @param include_triggered 是否包含已触发的
 * @return 数量
 */
int reminder_count(int include_triggered);

/**
 * @brief 清理提醒系统
 */
void reminder_cleanup(void);

/**
 * @brief 获取状态名称
 * @param status 状态枚举
 * @return 名称字符串
 */
const char* reminder_status_name(reminder_status_t status);

#endif /* AI_REMINDER_AI_REMINDER_H */