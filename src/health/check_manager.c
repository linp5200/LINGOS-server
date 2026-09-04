/**
 * @file    src/health/check_manager.c
 * @brief   自检管理器实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "check_manager.h"
#include "check_cache.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define MAX_CHECK_ITEMS 64

static check_item_t *g_items[MAX_CHECK_ITEMS];
static int g_item_count = 0;
static check_summary_t g_last_summary;
static char g_error[256] = {0};
static int g_initialized = 0;
static int g_cache_valid = 0;

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
    LOG_ERROR_T("CheckManager", "Error", "Set", "%s", g_error);
}

static void clear_summary(check_summary_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(check_summary_t));
    s->details[0] = '\0';
}

/* ============================================================
 * 初始化
 * ============================================================ */
int check_manager_init(void) {
    if (g_initialized) return 0;
    LOG_INFO_T("CheckManager", "Init", "Enter", "initializing check manager");
    g_item_count = 0;
    g_initialized = 1;
    clear_summary(&g_last_summary);
    check_cache_init();
    LOG_INFO_T("CheckManager", "Init", "OK", "check manager initialized");
    return 0;
}

/* ============================================================
 * 注册检查项
 * ============================================================ */
int check_manager_register(check_item_t *item) {
    if (!item || !item->id || !item->func) {
        set_error("Invalid check item (missing id or func)");
        return -1;
    }

    if (g_item_count >= MAX_CHECK_ITEMS) {
        set_error("Too many check items (max %d)", MAX_CHECK_ITEMS);
        return -1;
    }

    // 检查是否已存在
    for (int i = 0; i < g_item_count; i++) {
        if (strcmp(g_items[i]->id, item->id) == 0) {
            set_error("Check item '%s' already registered", item->id);
            return -1;
        }
    }

    g_items[g_item_count] = item;
    item->last_run = 0;
    item->last_result = CHECK_RESULT_SKIP;
    item->last_message[0] = '\0';
    g_item_count++;

    LOG_DEBUG_T("CheckManager", "Register", "OK", "registered '%s' (priority=%d)", item->id, item->priority);
    return 0;
}

/* ============================================================
 * 执行单个检查项
 * ============================================================ */
static int run_item(check_item_t *item, check_result_t *out_result) {
    if (!item) return -1;

    item->last_run = time(NULL);
    int ret = item->func();
    check_result_t result;

    switch (ret) {
        case 0:  result = CHECK_RESULT_PASS; break;
        case 1:  result = CHECK_RESULT_WARN; break;
        case 2:  result = CHECK_RESULT_FAIL; break;
        default: result = CHECK_RESULT_ERROR; break;
    }

    item->last_result = result;
    if (out_result) *out_result = result;

    // 尝试从缓存获取消息（如果有）
    char msg[256];
    if (check_cache_get(item->id, msg, sizeof(msg)) == 0) {
        safe_strncpy(item->last_message, msg, sizeof(item->last_message));
    } else {
        const char *status_str = "";
        switch (result) {
            case CHECK_RESULT_PASS: status_str = tr("PASS", "通过"); break;
            case CHECK_RESULT_WARN: status_str = tr("WARN", "警告"); break;
            case CHECK_RESULT_FAIL: status_str = tr("FAIL", "失败"); break;
            case CHECK_RESULT_SKIP: status_str = tr("SKIP", "跳过"); break;
            default:                status_str = tr("ERROR", "错误"); break;
        }
        safe_snprintf(item->last_message, sizeof(item->last_message),
                     "%s: %s", item->name_en, status_str);
    }

    LOG_DEBUG_T("CheckManager", "RunItem", "Result", "'%s' = %d", item->id, result);
    return (result == CHECK_RESULT_FAIL || result == CHECK_RESULT_ERROR) ? -1 : 0;
}

/* ============================================================
 * 汇总结果
 * ============================================================ */
static void aggregate_results(check_summary_t *summary) {
    if (!summary) return;
    clear_summary(summary);

    for (int i = 0; i < g_item_count; i++) {
        check_item_t *item = g_items[i];
        if (!item) continue;
        summary->total++;
        switch (item->last_result) {
            case CHECK_RESULT_PASS:  summary->passed++; break;
            case CHECK_RESULT_WARN:  summary->warned++; break;
            case CHECK_RESULT_FAIL:  summary->failed++; break;
            case CHECK_RESULT_SKIP:  summary->skipped++; break;
            case CHECK_RESULT_ERROR: summary->errors++; break;
            default: break;
        }
        if (strcmp(item->id, "config") == 0 && item->last_result == CHECK_RESULT_FAIL) {
            summary->need_configuration = 1;
        }
        char line[128];
        const char *status_str = "";
        switch (item->last_result) {
            case CHECK_RESULT_PASS:  status_str = "✅"; break;
            case CHECK_RESULT_WARN:  status_str = "⚠️"; break;
            case CHECK_RESULT_FAIL:  status_str = "❌"; break;
            case CHECK_RESULT_SKIP:  status_str = "⏭️"; break;
            default:                 status_str = "❓"; break;
        }
        safe_snprintf(line, sizeof(line), "  %s %s: %s\n",
                      status_str, item->name_en, item->last_message);
        safe_strlcat(summary->details, line, sizeof(summary->details));
    }

    // 缓存到磁盘
    check_cache_save(summary);
    g_cache_valid = 1;
}

/* ============================================================
 * 运行检查（内部通用）
 * ============================================================ */
static int run_checks(int quick_only, check_summary_t *summary) {
    if (!g_initialized) {
        set_error("Check manager not initialized");
        return -1;
    }

    clear_summary(&g_last_summary);

    int has_failure = 0;
    for (int i = 0; i < g_item_count; i++) {
        check_item_t *item = g_items[i];
        if (!item) continue;
        if (!item->enabled) {
            item->last_result = CHECK_RESULT_SKIP;
            continue;
        }
        if (quick_only && item->priority > CHECK_PRIORITY_HIGH) {
            // 快速模式下跳过低优先级
            item->last_result = CHECK_RESULT_SKIP;
            continue;
        }
        check_result_t result;
        if (run_item(item, &result) != 0 && result == CHECK_RESULT_FAIL) {
            has_failure = 1;
        }
    }

    aggregate_results(&g_last_summary);
    if (summary) {
        *summary = g_last_summary;
    }

    if (g_last_summary.failed > 0 || g_last_summary.errors > 0) {
        LOG_WARN_T("CheckManager", "Run", "HasFailures", "failed=%d, errors=%d",
                   g_last_summary.failed, g_last_summary.errors);
        return -1;
    }
    return 0;
}

/* ============================================================
 * API 实现
 * ============================================================ */
int check_manager_run_all(check_summary_t *summary) {
    return run_checks(0, summary);
}

int check_manager_run_quick(check_summary_t *summary) {
    return run_checks(1, summary);
}

int check_manager_run_one(const char *id, check_result_t *result) {
    if (!id) return -1;
    check_item_t *item = NULL;
    for (int i = 0; i < g_item_count; i++) {
        if (strcmp(g_items[i]->id, id) == 0) {
            item = g_items[i];
            break;
        }
    }
    if (!item) {
        set_error("Check item '%s' not found", id);
        return -1;
    }
    if (!item->enabled) {
        if (result) *result = CHECK_RESULT_SKIP;
        return 0;
    }
    return run_item(item, result);
}

const check_item_t* check_manager_get_item(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < g_item_count; i++) {
        if (strcmp(g_items[i]->id, id) == 0) {
            return g_items[i];
        }
    }
    return NULL;
}

const check_summary_t* check_manager_get_last_summary(void) {
    return &g_last_summary;
}

int check_manager_all_critical_passed(void) {
    for (int i = 0; i < g_item_count; i++) {
        check_item_t *item = g_items[i];
        if (!item) continue;
        if (item->priority == CHECK_PRIORITY_CRITICAL &&
            item->last_result != CHECK_RESULT_PASS &&
            item->last_result != CHECK_RESULT_WARN) {
            return 0;
        }
    }
    return 1;
}

int check_manager_need_configuration(void) {
    return g_last_summary.need_configuration;
}

void check_manager_invalidate_cache(void) {
    g_cache_valid = 0;
    check_cache_invalidate();
}

const char* check_manager_get_error(void) {
    return g_error;
}