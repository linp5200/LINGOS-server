/**
 * @file    ai_reminder_store.h
 * @brief   提醒持久化存储接口
 * @version LN-B-4.2.0.0
 */

#ifndef AI_REMINDER_AI_REMINDER_STORE_H
#define AI_REMINDER_AI_REMINDER_STORE_H

#include "ai_reminder.h"
#include <time.h>

/**
 * @brief 加载提醒存储（从索引文件）
 * @return 0 成功，-1 失败
 */
int reminder_store_load(void);

/**
 * @brief 保存单条提醒
 * @param reminder 提醒结构
 * @return 0 成功，-1 失败
 */
int reminder_store_save(const reminder_t *reminder);

/**
 * @brief 删除提醒
 * @param id 提醒 ID
 * @return 0 成功，-1 失败
 */
int reminder_store_delete(const char *id);

/**
 * @brief 获取单条提醒
 * @param id 提醒 ID
 * @param out 输出结构
 * @return 0 成功，-1 失败
 */
int reminder_store_get(const char *id, reminder_t *out);

/**
 * @brief 列出提醒
 * @param out 输出数组
 * @param max_count 最大数量
 * @param include_triggered 是否包含已触发的
 * @return 实际数量
 */
int reminder_store_list(reminder_t *out, int max_count, int include_triggered);

/**
 * @brief 获取提醒数量（统计）
 * @param include_triggered 是否包含已触发的
 * @return 数量
 */
int reminder_store_count(int include_triggered);

/**
 * @brief 清空提醒存储（重置索引）
 */
void reminder_store_clear(void);

#endif /* AI_REMINDER_AI_REMINDER_STORE_H */