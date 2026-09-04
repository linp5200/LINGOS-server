/**
 * @file    src/ui/startup_ui.c
 * @brief   启动界面 UI 辅助函数实现
 * @version LN-0.4.3
 * @changes 集成进度条系统 (progress_bar.h)；
 *          新增详细进度显示函数；
 *          所有用户可见输出双文支持
 */

#include "startup_ui.h"
#include "progress_bar.h"
#include "../config/config_core.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../common/version.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/* 当前启动步骤描述 */
static char g_startup_step[128] = {0};
static int g_startup_progress = 0;
static time_t g_startup_step_time = 0;

/* 进度条上下文 (用于详细进度显示) */
static progress_ctx_t g_progress_ctx;

/* ============================================================
 * 原有函数 (保持不变)
 * ============================================================ */

void ui_show_install_progress(const char *pkg_name, int percent, progress_status_t status) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    const char *color = COLOR_RESET;
    switch (status) {
        case PROGRESS_RUNNING: color = COLOR_CYAN; break;
        case PROGRESS_DONE:    color = COLOR_GREEN; break;
        case PROGRESS_FAILED:  color = COLOR_RED; break;
        default:               color = COLOR_DIM; break;
    }

    log_draw_progress(percent, pkg_name, status);
}

void ui_show_network_error(const char *reason) {
    if (!reason) reason = tr("Unknown network error", "未知网络错误");
    uart_puts(COLOR_YELLOW);
    uart_puts(tr("\n[Network] ", "[网络] "));
    uart_puts(tr("Network unavailable (", "网络不可用 ("));
    uart_puts(reason);
    uart_puts(tr(")\n", ")\n"));
    uart_puts(tr("Please check your network connection. Dependencies installation will be skipped.\n",
                 "请检查网络连接，即将跳过依赖安装。\n"));
    uart_puts(COLOR_RESET);
}

void ui_startup_message(const char *fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    uart_puts(msg);
    uart_puts("\n");
}

/* ============================================================
 * 启动进度和步骤显示 (原有)
 * ============================================================ */

void ui_show_startup_step(const char *step) {
    if (!step) return;
    safe_strncpy(g_startup_step, step, sizeof(g_startup_step));
    g_startup_step_time = time(NULL);
    uart_puts(COLOR_DIM);
    uart_puts("[");
    uart_puts(step);
    uart_puts("] ");
    uart_puts(COLOR_RESET);
    fflush(stdout);
}

void ui_show_startup_progress(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_startup_progress = percent;

    const char *color = COLOR_CYAN;
    if (percent >= 100) color = COLOR_GREEN;
    else if (percent < 30) color = COLOR_DIM;

    uart_puts("\r");
    uart_puts(color);
    uart_puts("[");
    int bar_width = 30;
    int filled = (percent * bar_width) / 100;
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) uart_puts("█");
        else uart_puts(" ");
    }
    uart_puts("] ");
    char pct_str[8];
    safe_snprintf(pct_str, sizeof(pct_str), "%3d%%", percent);
    uart_puts(pct_str);
    uart_puts(COLOR_RESET);
    if (g_startup_step[0]) {
        uart_puts(" ");
        uart_puts(COLOR_DIM);
        uart_puts(g_startup_step);
        uart_puts(COLOR_RESET);
    }
    fflush(stdout);
}

void show_startup_banner(void) {
    char banner_en[1024];
    char banner_zh[1024];

    /* 【2026-08-23 修复】已配置完成 → 简短横幅（不再显示"首次启动下载模型"误导提示） */
    int configured = config_core_is_configured();

    if (configured) {
        snprintf(banner_en, sizeof(banner_en),
            "\n╔════════════════════════════════════════════════════════════╗\n"
            "║                    LING OS %-12s                  ║\n"
            "║                    Starting up...                         ║\n"
            "╚════════════════════════════════════════════════════════════╝\n",
            LINGOS_VERSION);
        snprintf(banner_zh, sizeof(banner_zh),
            "\n╔════════════════════════════════════════════════════════════╗\n"
            "║                    LING OS %-12s                  ║\n"
            "║                    正在启动...                             ║\n"
            "╚════════════════════════════════════════════════════════════╝\n",
            LINGOS_VERSION);
    } else {
        snprintf(banner_en, sizeof(banner_en),
            "\n╔════════════════════════════════════════════════════════════╗\n"
            "║                    LING OS %-12s                  ║\n"
            "║            Starting up, please wait...                    ║\n"
            "║                                                           ║\n"
            "║  First startup may take 5-20 minutes to download models.  ║\n"
            "║  Progress will be shown below.                           ║\n"
            "╚════════════════════════════════════════════════════════════╝\n",
            LINGOS_VERSION);
        snprintf(banner_zh, sizeof(banner_zh),
            "\n╔════════════════════════════════════════════════════════════╗\n"
            "║                    LING OS %-12s                  ║\n"
            "║            正在启动，请稍候...                             ║\n"
            "║                                                           ║\n"
            "║  首次启动可能需要5-20分钟下载模型。                       ║\n"
            "║  进度将在下方显示。                                       ║\n"
            "╚════════════════════════════════════════════════════════════╝\n",
            LINGOS_VERSION);
    }

    uart_puts(COLOR_CYAN);
    uart_puts(tr(banner_en, banner_zh));
    uart_puts(COLOR_RESET);
    uart_puts("\n");
    fflush(stdout);
}

int ui_get_startup_progress(void) {
    return g_startup_progress;
}

const char* ui_get_startup_step(void) {
    return g_startup_step;
}

/* ============================================================
 * 新增：详细进度条 (基于 progress_bar 系统)
 * ============================================================ */

/**
 * @brief 初始化详细进度
 */
void ui_init_detailed_progress(const char *name, progress_type_t type,
                               int total_items, int has_speed) {
    progress_bar_init(&g_progress_ctx, name, type, total_items, has_speed, 50);
    LOG_DEBUG_T("StartupUI", "InitDetailed", "OK", "name='%s', type=%d", name, type);
}

/**
 * @brief 更新详细进度
 */
void ui_update_detailed_progress(int progress, double speed,
                                 double downloaded, double total_size) {
    progress_bar_update(&g_progress_ctx, progress, speed, downloaded, total_size);
    progress_bar_render(&g_progress_ctx);
}

/**
 * @brief 设置当前项目编号
 */
void ui_set_detailed_item(int current_item) {
    progress_bar_set_item(&g_progress_ctx, current_item);
}

/**
 * @brief 完成详细进度
 */
void ui_finish_detailed_progress(int success, const char *message) {
    progress_bar_finish(&g_progress_ctx, success, message);
}

/**
 * @brief 重置详细进度 (用于下一个操作)
 */
void ui_reset_detailed_progress(void) {
    memset(&g_progress_ctx, 0, sizeof(progress_ctx_t));
}

/**
 * @brief 获取当前详细进度上下文 (供 env_bootstrap 直接操作)
 */
progress_ctx_t* ui_get_progress_ctx(void) {
    return &g_progress_ctx;
}

/**
 * @brief 显示无速度概念步骤状态
 * @param step_name 步骤名称
 * @param status 状态 (OK/copying/setting up/making)
 * @param progress 进度百分比
 */
void ui_show_step_status(const char *step_name, const char *status, int progress) {
    if (!step_name) return;

    const char *color = COLOR_CYAN;
    const char *status_icon = "🔄";

    if (strcmp(status, "OK") == 0 || strcmp(status, "ok") == 0) {
        color = COLOR_GREEN;
        status_icon = "✅";
    } else if (strcmp(status, "FAILED") == 0 || strcmp(status, "failed") == 0) {
        color = COLOR_RED;
        status_icon = "❌";
    }

    char line[256];
    safe_snprintf(line, sizeof(line), "%s%s %s%s%s%s",
                  color, status_icon, step_name,
                  status ? " - " : "",
                  status ? status : "",
                  COLOR_RESET);

    /* 如果有进度，添加到行尾 */
    if (progress >= 0 && progress <= 100) {
        char pct[8];
        safe_snprintf(pct, sizeof(pct), " (%d%%)", progress);
        safe_strlcat(line, pct, sizeof(line));
    }

    uart_puts("\r");
    uart_puts(line);

    /* 清空行尾 */
    int term_width = progress_bar_get_terminal_width();
    int len = strlen(line);
    if (len < term_width) {
        for (int i = len; i < term_width; i++) {
            uart_puts(" ");
        }
    }

    fflush(stdout);

    /* 如果是完成状态，换行 */
    if (strcmp(status, "OK") == 0 || strcmp(status, "FAILED") == 0) {
        uart_puts("\n");
    }
}