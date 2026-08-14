/**
 * @file    src/install/install_speed.h
 * @brief   速度自动计算
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_SPEED_H
#define INSTALL_SPEED_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 速度计算器结构
 * ============================================================ */
typedef struct speed_calc {
    size_t prev_bytes;
    time_t prev_time;
    double current_speed;   /* MB/s */
    int is_valid;
    int initialized;
} speed_calc_t;

/**
 * @brief 初始化速度计算器
 * @param calc 计算器指针
 */
void speed_calc_init(speed_calc_t *calc);

/**
 * @brief 更新速度计算
 * @param calc 计算器指针
 * @param total_bytes 当前已下载总字节数
 * @return 当前速度 (MB/s)
 */
double speed_calc_update(speed_calc_t *calc, size_t total_bytes);

/**
 * @brief 获取当前速度
 * @param calc 计算器指针
 * @return 当前速度 (MB/s)
 */
double speed_calc_get(speed_calc_t *calc);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_SPEED_H */