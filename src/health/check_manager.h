/**
 * @file    src/health/check_manager.h
 * @brief   自检管理器：注册、执行、结果聚合
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef HEALTH_CHECK_MANAGER_H
#define HEALTH_CHECK_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 检查项优先级
 * ============================================================ */
typedef enum {
    CHECK_PRIORITY_CRITICAL = 0,   /* 必须通过，否则阻止启动 */
    CHECK_PRIORITY_HIGH = 1,       /* 重要，失败则警告 */
    CHECK_PRIORITY_NORMAL = 2,     /* 普通，仅记录 */
    CHECK_PRIORITY_LOW = 3         /* 低优先级，可跳过 */
} check_priority_t;

/* ============================================================
 * 检查结果状态
 * ============================================================ */
typedef enum {
    CHECK_RESULT_PASS = 0,         /* 通过 */
    CHECK_RESULT_WARN = 1,         /* 警告（可通过） */
    CHECK_RESULT_FAIL = 2,         /* 失败（需处理） */
    CHECK_RESULT_SKIP = 3,         /* 跳过（未执行） */
    CHECK_RESULT_ERROR = 4         /* 检查自身出错 */
} check_result_t;

/* ============================================================
 * 检查项结构
 * ============================================================ */
typedef struct check_item {
    const char *id;                /* 唯一标识符（如 "lang", "deps", "config"） */
    const char *name_en;           /* 英文名称 */
    const char *name_zh;           /* 中文名称 */
    check_priority_t priority;     /* 优先级 */
    int (*func)(void);             /* 执行检查的函数，返回 check_result_t */
    int (*fix_func)(void);         /* 修复函数（可选），返回 0 成功 */
    int enabled;                   /* 是否启用 */
    time_t last_run;               /* 上次执行时间 */
    check_result_t last_result;    /* 上次结果 */
    char last_message[256];        /* 上次结果消息 */
} check_item_t;

/* ============================================================
 * 检查结果汇总
 * ============================================================ */
typedef struct check_summary {
    int total;                     /* 总检查数 */
    int passed;                    /* 通过数 */
    int warned;                    /* 警告数 */
    int failed;                    /* 失败数 */
    int skipped;                   /* 跳过数 */
    int errors;                    /* 错误数 */
    int need_configuration;        /* 是否需要配置（由检查项设置） */
    char details[4096];            /* 详细结果文本 */
} check_summary_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * @brief 初始化检查管理器
 * @return 0 成功，-1 失败
 */
int check_manager_init(void);

/**
 * @brief 注册检查项
 * @param item 检查项定义（指针需持久化）
 * @return 0 成功，-1 失败
 */
int check_manager_register(check_item_t *item);

/**
 * @brief 运行所有检查项
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 全部通过或警告，-1 有失败项
 */
int check_manager_run_all(check_summary_t *summary);

/**
 * @brief 运行指定检查项
 * @param id 检查项 ID
 * @param result 输出结果（可为 NULL）
 * @return 0 成功，-1 失败
 */
int check_manager_run_one(const char *id, check_result_t *result);

/**
 * @brief 运行快速检查（仅关键和高优先级）
 * @param summary 输出汇总结果（可为 NULL）
 * @return 0 全部通过或警告，-1 有失败项
 */
int check_manager_run_quick(check_summary_t *summary);

/**
 * @brief 获取检查项状态
 * @param id 检查项 ID
 * @return 检查项指针，未找到返回 NULL
 */
const check_item_t* check_manager_get_item(const char *id);

/**
 * @brief 获取最近一次汇总结果
 * @return 汇总结果指针（只读）
 */
const check_summary_t* check_manager_get_last_summary(void);

/**
 * @brief 检查是否所有关键检查通过
 * @return 1 全部通过，0 有失败
 */
int check_manager_all_critical_passed(void);

/**
 * @brief 检查是否需要配置
 * @return 1 需要配置，0 不需要
 */
int check_manager_need_configuration(void);

/**
 * @brief 清空缓存（强制重新检查所有项）
 */
void check_manager_invalidate_cache(void);

/**
 * @brief 获取错误消息
 * @return 错误消息字符串
 */
const char* check_manager_get_error(void);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_CHECK_MANAGER_H */