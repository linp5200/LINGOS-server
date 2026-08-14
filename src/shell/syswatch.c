/**
 * @file    syswatch.c
 * @brief   系统看门狗：监控系统是否卡死，提供任务状态查询
 * @version 2.2.0.0
 */

#include "syswatch.h"
#include "log_extra.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>

static int interval = 10;
static volatile int fed = 0;
static volatile int command_running = 0;

void syswatch_init(int interval_sec) {
    interval = interval_sec;
    fed = 1;
    command_running = 0;
    LOG_INFO_T("SysWatch", "Init", "OK", "watchdog initialized with %d seconds", interval);
}

void syswatch_feed(void) {
    fed = 1;
}

void syswatch_start_command(void) {
    command_running = 1;
}

void syswatch_end_command(void) {
    command_running = 0;
}

int syswatch_is_busy(void) {
    return command_running;
}