/**
 * @file    alert_cmds.c
 * @brief   alert 子命令实现
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程
 */

#include "alert_cmds.h"
#include "../alert/alert_manager.h"
#include "../alert/alert_config.h"
#include "../alert/alert_history.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 显示预警状态
 * ============================================================ */

static void cmd_alert_status(void) {
    alert_event_t events[8];
    int count = alert_manager_get_latest(events, 8);

    uart_puts(tr("\n=== Alert Status ===\n", "\n=== 预警状态 ===\n"));

    if (count == 0) {
        uart_puts(tr("No active alerts.\n", "没有活跃的预警。\n"));
        return;
    }

    const char *type_names[] = {"Unknown", "Typhoon", "Earthquake", "Rain", "High Temp", "Storm", "Fire"};
    const char *level_names[] = {"Info", "Blue", "Yellow", "Orange", "Red", "Critical"};

    for (int i = 0; i < count; i++) {
        alert_event_t *ev = &events[i];
        char time_buf[32];
        struct tm *tm = localtime(&ev->timestamp);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);

        char buf[256];
        safe_snprintf(buf, sizeof(buf),
                      "  [%s] %s: %s (Level %s)\n"
                      "    Location: %s, Source: %s\n"
                      "    Time: %s\n",
                      time_buf,
                      type_names[ev->type],
                      ev->description,
                      level_names[ev->level],
                      ev->location,
                      ev->source,
                      time_buf);
        uart_puts(buf);
        uart_puts("\n");
    }
}

/* ============================================================
 * 显示配置
 * ============================================================ */

static void cmd_alert_config_show(void) {
    alert_config_t cfg;
    alert_config_load(&cfg);
    alert_config_validate(&cfg);

    uart_puts(tr("\n=== Alert Configuration ===\n", "\n=== 预警配置 ===\n"));
    char buf[128];
    safe_snprintf(buf, sizeof(buf), "Base interval: %ds\n", cfg.base_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Exception interval: %ds\n", cfg.exception_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Realtime interval: %ds\n", cfg.realtime_interval);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Typhoon distance threshold: %dkm\n", cfg.typhoon_distance_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Typhoon level threshold: %d\n", cfg.typhoon_level_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Earthquake magnitude threshold: %.1f\n", cfg.earthquake_magnitude_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "Rainfall threshold: %.1fmm/24h\n", cfg.rainfall_threshold);
    uart_puts(buf);
}

/* ============================================================
 * 查看历史
 * ============================================================ */

static void cmd_alert_history(int limit) {
    alert_event_t events[32];
    int count = alert_history_query(NULL, NULL, 24*30, events, 32); /* 最近30天 */
    if (count > limit) count = limit;

    uart_puts(tr("\n=== Alert History ===\n", "\n=== 预警历史 ===\n"));

    if (count == 0) {
        uart_puts(tr("No history found.\n", "未找到历史记录。\n"));
        return;
    }

    const char *type_names[] = {"Unknown", "Typhoon", "Earthquake", "Rain", "High Temp", "Storm", "Fire"};
    const char *level_names[] = {"Info", "Blue", "Yellow", "Orange", "Red", "Critical"};

    for (int i = 0; i < count; i++) {
        alert_event_t *ev = &events[i];
        char time_buf[32];
        struct tm *tm = localtime(&ev->timestamp);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);

        char buf[128];
        safe_snprintf(buf, sizeof(buf),
                      "  %s: %s (Level %s) - %s\n",
                      time_buf,
                      type_names[ev->type],
                      level_names[ev->level],
                      ev->description);
        uart_puts(buf);
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */

void alert_dispatch(const char *args) {
    LOG_DEBUG_T("AlertCmds", "Dispatch", "Enter", "args=%s", args ? args : "(null)");

    if (!args || !*args) {
        uart_puts(tr("Usage: alert <subcommand>\n", "用法：alert <子命令>\n"));
        uart_puts(tr("  status       - Show current alerts\n", "  status       - 显示当前预警\n"));
        uart_puts(tr("  config       - Show configuration\n", "  config       - 显示配置\n"));
        uart_puts(tr("  history [N]  - Show history (last N entries)\n", "  history [N]  - 显示历史（最近 N 条）\n"));
        return;
    }

    if (strcmp(args, "status") == 0) {
        cmd_alert_status();
    } else if (strcmp(args, "config") == 0) {
        cmd_alert_config_show();
    } else if (strncmp(args, "history", 7) == 0) {
        int limit = 10;
        if (args[7] == ' ') {
            limit = atoi(args + 8);
            if (limit < 1) limit = 10;
        }
        cmd_alert_history(limit);
    } else {
        uart_puts(tr("Unknown alert subcommand.\n", "未知的 alert 子命令。\n"));
    }
}