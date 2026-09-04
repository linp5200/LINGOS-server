/**
 * @file    src/install/install_speed.c
 * @brief   速度自动计算实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "install_speed.h"
#include <string.h>
#include <time.h>

/* ============================================================
 * 初始化速度计算器
 * ============================================================ */
void speed_calc_init(speed_calc_t *calc) {
    if (!calc) return;
    calc->prev_bytes = 0;
    calc->prev_time = time(NULL);
    calc->current_speed = 0;
    calc->is_valid = 0;
    calc->initialized = 1;
}

/* ============================================================
 * 更新速度计算
 * ============================================================ */
double speed_calc_update(speed_calc_t *calc, size_t total_bytes) {
    if (!calc || !calc->initialized) return 0.0;

    time_t now = time(NULL);
    double elapsed = difftime(now, calc->prev_time);

    if (elapsed >= 0.5) {
        size_t diff = total_bytes - calc->prev_bytes;
        calc->current_speed = (diff / 1024.0 / 1024.0) / elapsed;
        calc->is_valid = 1;
        calc->prev_bytes = total_bytes;
        calc->prev_time = now;
    }

    return calc->current_speed;
}

/* ============================================================
 * 获取当前速度
 * ============================================================ */
double speed_calc_get(speed_calc_t *calc) {
    if (!calc) return 0.0;
    return calc->current_speed;
}