/**
 * @file    src/core/exit_status.h
 * @brief   退出状态管理头文件（检测异常关闭）
 * @version LN-B-5.1.2.6-rc
 */

#ifndef CORE_EXIT_STATUS_H
#define CORE_EXIT_STATUS_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 退出状态结构
 * ============================================================ */

typedef struct {
    time_t last_start_time;      /* 上次启动时间 */
    time_t last_exit_time;       /* 上次退出时间 */
    int last_exit_code;          /* 上次退出码 */
    int is_clean_exit;           /* 1=正常退出, 0=异常退出 */
    char last_exit_reason[128];  /* 退出原因描述 */
    int crash_count;             /* 连续异常退出次数 */
    time_t first_crash_time;     /* 首次异常退出时间 */
} exit_status_t;

/* ============================================================
 * 核心 API
 * ============================================================ */

/**
 * @brief 初始化退出状态（在 main() 开始时调用）
 * @param status 退出状态结构指针
 * @return 0 成功，-1 失败
 */
int exit_status_init(exit_status_t *status);

/**
 * @brief 检查上次是否正常退出
 * @param status 退出状态结构指针
 * @return 1 异常退出，0 正常退出
 */
int exit_status_check_abnormal(exit_status_t *status);

/**
 * @brief 标记正常退出（在程序正常退出时调用）
 * @param exit_code 退出码
 * @param reason 退出原因（可为 NULL）
 */
void exit_status_mark_clean(int exit_code, const char *reason);

/**
 * @brief 标记异常退出（在程序异常退出时调用，如信号处理）
 * @param signal 信号编号
 * @param reason 退出原因（可为 NULL）
 */
void exit_status_mark_abnormal(int signal, const char *reason);

/**
 * @brief 清除异常标记（当用户选择"忽略并继续"时调用）
 */
void exit_status_clear_abnormal(void);

/**
 * @brief 获取当前退出状态
 * @return 退出状态结构指针（静态分配）
 */
const exit_status_t* exit_status_get(void);

/**
 * @brief 获取退出状态的友好描述
 * @param status 退出状态结构指针
 * @param lang 语言 ("en" 或 "zh")
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void exit_status_format_message(const exit_status_t *status,
                                const char *lang,
                                char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CORE_EXIT_STATUS_H */