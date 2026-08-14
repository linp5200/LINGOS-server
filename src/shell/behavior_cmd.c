/**
 * @file    behavior_cmd.c
 * @brief   system behavior 命令实现
 * @version LN-B-5.0.0.0
 */

#include "../health/behavior_monitor.h"
#include "../security/security_config.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ============================================================
 * 显示行为监控状态
 * ============================================================ */
static void cmd_behavior_status(void) {
    const security_config_t *cfg = security_config_get();
    behavior_algorithm_t algo = behavior_monitor_get_algorithm();
    int learning = behavior_monitor_is_learning();

    uart_puts(tr("\n=== Behavior Monitoring Status ===\n", "\n=== 行为监控状态 ===\n"));

    char buf[128];

    safe_snprintf(buf, sizeof(buf),
                  tr("Enabled: %s\n", "启用状态：%s\n"),
                  cfg && cfg->behavior_enabled ? tr("Yes", "是") : tr("No", "否"));
    uart_puts(buf);

    safe_snprintf(buf, sizeof(buf),
                  tr("Algorithm: %s\n", "算法：%s\n"),
                  behavior_monitor_algorithm_name(algo));
    uart_puts(buf);

    safe_snprintf(buf, sizeof(buf),
                  tr("Learning mode: %s\n", "学习模式：%s\n"),
                  learning ? tr("Active", "激活") : tr("Complete", "已完成"));
    uart_puts(buf);

    if (cfg) {
        safe_snprintf(buf, sizeof(buf),
                      tr("Window size: %d\n", "窗口大小：%d\n"),
                      cfg->behavior_window_size);
        uart_puts(buf);
        safe_snprintf(buf, sizeof(buf),
                      tr("Threshold: %d\n", "阈值：%d\n"),
                      cfg->behavior_threshold);
        uart_puts(buf);
        safe_snprintf(buf, sizeof(buf),
                      tr("Auto-escalate: %s\n", "自动升级：%s\n"),
                      cfg->behavior_auto_escalate ? tr("Enabled", "启用") : tr("Disabled", "禁用"));
        uart_puts(buf);
    }
}

/* ============================================================
 * 切换算法
 * ============================================================ */
static void cmd_behavior_algorithm(const char *algo_name) {
    if (!algo_name) {
        uart_puts(tr("Usage: system behavior algorithm ewma|isolation_forest|hybrid\n",
                     "用法：system behavior algorithm ewma|isolation_forest|hybrid\n"));
        return;
    }

    behavior_algorithm_t algo;
    if (strcmp(algo_name, "ewma") == 0) {
        algo = ALGORITHM_EWMA;
    } else if (strcmp(algo_name, "isolation_forest") == 0) {
        algo = ALGORITHM_ISOLATION_FOREST;
        uart_puts(tr("Note: Isolation Forest is experimental. EWMA will be used as fallback.\n",
                     "注意：孤立森林为实验性功能，实际将使用 EWMA 作为降级方案。\n"));
    } else if (strcmp(algo_name, "hybrid") == 0) {
        algo = ALGORITHM_HYBRID;
        uart_puts(tr("Note: Hybrid mode is experimental. EWMA will be used as fallback.\n",
                     "注意：混合模式为实验性功能，实际将使用 EWMA 作为降级方案。\n"));
    } else {
        uart_puts(tr("Invalid algorithm. Must be ewma, isolation_forest, or hybrid.\n",
                     "无效的算法。必须是 ewma、isolation_forest 或 hybrid。\n"));
        return;
    }

    int ret = behavior_monitor_set_algorithm(algo);
    if (ret == 0) {
        char buf[128];
        safe_snprintf(buf, sizeof(buf),
                      tr("Algorithm set to: %s\n", "算法已设置为：%s\n"),
                      algo_name);
        uart_puts(buf);
    } else {
        uart_puts(tr("Failed to set algorithm.\n", "设置算法失败。\n"));
    }
}

/* ============================================================
 * 显示建议
 * ============================================================ */
static void cmd_behavior_suggest(void) {
    int data_count = behavior_monitor_get_data_count("default");
    const security_config_t *cfg = security_config_get();
    int min_samples = 500;

    uart_puts(tr("\n=== Behavior Monitoring Suggestions ===\n", "\n=== 行为监控建议 ===\n"));

    char buf[128];
    safe_snprintf(buf, sizeof(buf),
                  tr("Current data points: %d\n", "当前数据点：%d\n"),
                  data_count);
    uart_puts(buf);

    if (data_count < min_samples) {
        safe_snprintf(buf, sizeof(buf),
                      tr("Need %d more data points for isolation forest.\n",
                         "还需 %d 个数据点才能启用孤立森林。\n"),
                      min_samples - data_count);
        uart_puts(buf);
    } else {
        uart_puts(tr("✅ Data is sufficient for isolation forest.\n",
                     "✅ 数据量已足够启用孤立森林。\n"));
        uart_puts(tr("Try: system behavior algorithm isolation_forest\n",
                     "尝试：system behavior algorithm isolation_forest\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void behavior_dispatch(const char *args) {
    LOG_DEBUG_T("BehaviorCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args || strcmp(args, "status") == 0) {
        cmd_behavior_status();
        return;
    }

    char cmd_buf[128];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *param = strtok_r(NULL, " ", &saveptr);

    if (!subcmd) {
        cmd_behavior_status();
        return;
    }

    if (strcmp(subcmd, "algorithm") == 0) {
        cmd_behavior_algorithm(param);
        return;
    }

    if (strcmp(subcmd, "suggest") == 0 || strcmp(subcmd, "suggestion") == 0) {
        cmd_behavior_suggest();
        return;
    }

    uart_puts(tr("Unknown behavior subcommand.\n", "未知的 behavior 子命令。\n"));
    uart_puts(tr("Available: status, algorithm <name>, suggest\n",
                 "可用：status, algorithm <名称>, suggest\n"));
}