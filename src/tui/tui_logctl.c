/**
 * @file    tui_logctl.c
 * @brief   TUI 日志自动控制（进入静默，退出恢复）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程
 */

#include "tui_logctl.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 日志状态快照
 * ============================================================ */

static struct {
    int console_enabled;
    int file_enabled;
    int level;
} g_log_snapshot;

static int g_log_saved = 0;

/* ============================================================
 * 公共 API
 * ============================================================ */

void tui_logctl_suspend(void) {
    if (g_log_saved) {
        LOG_DEBUG_T("TuiLogCtl", "Suspend", "Already", "logs already suspended");
        return;
    }

    /* 保存当前状态（仅保存日志级别，控制台状态默认启用） */
    g_log_snapshot.console_enabled = 1;  /* 假设进入前控制台已启用 */
    g_log_snapshot.file_enabled = 1;     /* 保留，但未使用 */
    g_log_snapshot.level = log_get_level();

    /* 关闭控制台输出，降低日志级别（只保留错误） */
    log_set_console_output(0);
    log_set_level(LOG_LEVEL_ERROR);

    g_log_saved = 1;
    LOG_DEBUG_T("TuiLogCtl", "Suspend", "OK", "logs suspended");
}

void tui_logctl_restore(void) {
    if (!g_log_saved) {
        LOG_DEBUG_T("TuiLogCtl", "Restore", "NotSaved", "no snapshot to restore");
        return;
    }

    log_set_console_output(g_log_snapshot.console_enabled);
    log_set_level(g_log_snapshot.level);

    g_log_saved = 0;
    LOG_DEBUG_T("TuiLogCtl", "Restore", "OK", "logs restored");
}

int tui_logctl_is_suspended(void) {
    return g_log_saved;
}