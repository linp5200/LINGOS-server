#ifndef SHELL_SYSWATCH_H
#define SHELL_SYSWATCH_H

#include <stdint.h>

/**
 * @brief 初始化系统看门狗
 * @param interval_sec 检查间隔（秒）
 */
void syswatch_init(int interval_sec);

/**
 * @brief 喂狗（重置超时计数器）
 */
void syswatch_feed(void);

/**
 * @brief 标记命令开始执行
 */
void syswatch_start_command(void);

/**
 * @brief 标记命令执行结束
 */
void syswatch_end_command(void);

/**
 * @brief 获取当前是否有任务正在运行
 * @return 1 有任务，0 无任务
 */
int syswatch_is_busy(void);

#endif