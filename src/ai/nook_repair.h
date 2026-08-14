#ifndef AI_NOOK_REPAIR_H
#define AI_NOOK_REPAIR_H

#include <stdint.h>
#include <stddef.h>   /* 新增：为 size_t 提供定义 */

/* 修复状态枚举 */
typedef enum {
    REPAIR_STATUS_IDLE,
    REPAIR_STATUS_RUNNING,
    REPAIR_STATUS_SUCCESS,
    REPAIR_STATUS_FAILED,
    REPAIR_STATUS_ROLLBACK
} repair_status_t;

/* 错误严重程度 */
typedef enum {
    REPAIR_ERR_LOW,
    REPAIR_ERR_MEDIUM,
    REPAIR_ERR_HIGH,
    REPAIR_ERR_CRITICAL
} repair_severity_t;

/* 启动修复（由看门狗或日志解析触发）*/
int nook_repair_start(const char *error_desc, repair_severity_t severity, int auto_confirm);

/* 获取当前修复状态 */
repair_status_t nook_repair_get_status(void);

/* 获取状态字符串 */
const char* nook_repair_status_str(repair_status_t status);

/* 查询修复历史（通过 socket 与 Python 端通信）*/
int nook_repair_history(const char *id, char *out, size_t out_len);   /* 改为 size_t */

/* 初始化修复系统 */
void nook_repair_init(void);

const char* nook_repair_get_status_desc(void);
#endif