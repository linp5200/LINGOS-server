/**
 * @file    alertd.h
 * @brief   预警系统独立子进程头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_ALERTD_H
#define ALERT_ALERTD_H

#include <stdint.h>

/* 外部接口（供主进程监控使用） */
int alertd_health_check(void); /* 预留 */

#endif /* ALERT_ALERTD_H */