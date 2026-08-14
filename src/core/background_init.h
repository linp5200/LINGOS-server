/**
 * @file    src/core/background_init.h
 * @brief   后台初始化线程头文件
 * @version LN-B-5.1.2.6-rc
 */

#ifndef CORE_BACKGROUND_INIT_H
#define CORE_BACKGROUND_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动后台初始化线程（registry_init, config_load 等）
 * @return 0 成功，-1 失败
 */
int start_background_initialization(void);

/**
 * @brief 停止后台初始化线程（等待完成）
 */
void stop_background_initialization(void);

/**
 * @brief 查询后台初始化是否正在运行
 * @return 1 运行中，0 已完成或未启动
 */
int background_init_is_running(void);

/**
 * @brief 查询后台初始化是否已完成
 * @return 1 已完成，0 未完成
 */
int background_init_is_done(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_BACKGROUND_INIT_H */