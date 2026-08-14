/**
 * @file    ai_reminder_scheduler.h
 * @brief   提醒定时调度器接口
 * @version LN-B-4.2.0.0
 */

#ifndef AI_REMINDER_AI_REMINDER_SCHEDULER_H
#define AI_REMINDER_AI_REMINDER_SCHEDULER_H

#include "ai_reminder.h"

/**
 * @brief 启动调度器（后台线程）
 * @return 0 成功，-1 失败
 */
int reminder_scheduler_start(void);

/**
 * @brief 停止调度器
 */
void reminder_scheduler_stop(void);

/**
 * @brief 添加提醒到调度队列
 * @param reminder 提醒结构
 * @return 0 成功，-1 失败
 */
int reminder_scheduler_add(const reminder_t *reminder);

/**
 * @brief 从调度队列移除提醒
 * @param id 提醒 ID
 * @return 0 成功，-1 失败
 */
int reminder_scheduler_remove(const char *id);

/**
 * @brief 获取调度队列中的提醒数量
 * @return 数量
 */
int reminder_scheduler_count(void);

/**
 * @brief 检查调度器是否运行中
 * @return 1 运行中，0 未运行
 */
int reminder_scheduler_is_running(void);

#endif /* AI_REMINDER_AI_REMINDER_SCHEDULER_H */