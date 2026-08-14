/**
 * @file    src/ui/progress_bar.c
 * @brief   进度条系统实现（单行动态刷新）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：防弹编程
 */

#include "progress_bar.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

/* ============================================================
 * 常量定义
 * ============================================================ */

#define DEFAULT_WIDTH 50
#define MIN_WIDTH 10
#define MAX_WIDTH 80
#define SPINNER_COUNT 10

/* ============================================================
 * 旋转动画字符集
 * ============================================================ */

static const char *g_spinner_chars[SPINNER_COUNT] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

/* ============================================================
 * 进度条填充字符集 (渐进式)
 * ============================================================ */

static const char *g_progress_chars[] = {
    " ",   /* 0% 空 */
    "▎",   /* 12.5% */
    "▍",   /* 25% */
    "▌",   /* 37.5% */
    "▋",   /* 50% */
    "▊",   /* 62.5% */
    "▉",   /* 75% */
    "█"    /* 87.5%+ 满 */
};

#define PROGRESS_CHAR_COUNT (sizeof(g_progress_chars) / sizeof(g_progress_chars[0]))

/* ============================================================
 * 辅助：获取终端宽度
 * ============================================================ */

int progress_bar_get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    const char *cols = getenv("COLUMNS");
    if (cols) {
        int val = atoi(cols);
        if (val > 0) return val;
    }
    return 80;
}

/* ============================================================
 * 辅助：格式化大小
 * ============================================================ */

void progress_bar_format_size(double bytes, char *buf, size_t size) {
    if (!buf || size == 0) return;

    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double val = bytes;

    if (val < 0) {
        safe_strncpy(buf, "N/A", size);
        return;
    }

    while (val >= 1024.0 && unit_index < 4) {
        val /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        safe_snprintf(buf, size, "%.0f%s", val, units[unit_index]);
    } else if (val >= 100) {
        safe_snprintf(buf, size, "%.0f%s", val, units[unit_index]);
    } else if (val >= 10) {
        safe_snprintf(buf, size, "%.1f%s", val, units[unit_index]);
    } else {
        safe_snprintf(buf, size, "%.2f%s", val, units[unit_index]);
    }
}

/* ============================================================
 * 辅助：格式化时间
 * ============================================================ */

void progress_bar_format_time(int seconds, char *buf, size_t size) {
    if (!buf || size == 0) return;

    if (seconds < 0) {
        safe_strncpy(buf, "N/A", size);
        return;
    }

    if (seconds < 60) {
        safe_snprintf(buf, size, "%ds", seconds);
    } else if (seconds < 3600) {
        int mins = seconds / 60;
        int secs = seconds % 60;
        safe_snprintf(buf, size, "%dm%02ds", mins, secs);
    } else {
        int hours = seconds / 3600;
        int mins = (seconds % 3600) / 60;
        safe_snprintf(buf, size, "%dh%02dm", hours, mins);
    }
}

/* ============================================================
 * 辅助：获取旋转动画字符
 * ============================================================ */

const char* progress_bar_get_spinner_char(int frame) {
    return g_spinner_chars[frame % SPINNER_COUNT];
}

/* ============================================================
 * 辅助：计算进度条填充
 * ============================================================ */

static void render_progress_bar(const progress_ctx_t *ctx, char *buf, size_t size) {
    int width = ctx->width;
    if (width < MIN_WIDTH) width = MIN_WIDTH;
    if (width > MAX_WIDTH) width = MAX_WIDTH;

    int progress = ctx->progress;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    int filled = (progress * width) / 100;
    int partial_index = ((progress * width) % 100) * PROGRESS_CHAR_COUNT / 100;

    if (partial_index >= PROGRESS_CHAR_COUNT) partial_index = PROGRESS_CHAR_COUNT - 1;

    int pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < width; i++) {
        if (i < filled) {
            buf[pos++] = g_progress_chars[PROGRESS_CHAR_COUNT - 1][0];
        } else if (i == filled && partial_index > 0 && filled < width) {
            buf[pos++] = g_progress_chars[partial_index][0];
        } else {
            buf[pos++] = ' ';
        }
    }

    buf[pos++] = ']';
    buf[pos] = '\0';
}

/* ============================================================
 * 核心 API 实现
 * ============================================================ */

void progress_bar_init(progress_ctx_t *ctx, const char *name,
                       progress_type_t type, int total_items,
                       int has_speed, int width) {
    if (!ctx) return;

    memset(ctx, 0, sizeof(progress_ctx_t));

    if (name) {
        safe_strncpy(ctx->name, name, sizeof(ctx->name));
    } else {
        safe_strncpy(ctx->name, "Unknown", sizeof(ctx->name));
    }

    ctx->type = type;
    ctx->total_items = total_items > 0 ? total_items : 1;
    ctx->current_item = 1;
    ctx->has_speed = has_speed;
    ctx->width = width > 0 ? width : DEFAULT_WIDTH;

    if (ctx->width < MIN_WIDTH) ctx->width = MIN_WIDTH;
    if (ctx->width > MAX_WIDTH) ctx->width = MAX_WIDTH;

    ctx->start_time = time(NULL);
    ctx->last_progress = -1;
    ctx->frame_count = 0;

    LOG_DEBUG_T("ProgressBar", "Init", "OK", "name='%s', type=%d, total=%d, has_speed=%d",
                ctx->name, ctx->type, ctx->total_items, ctx->has_speed);
}

void progress_bar_update(progress_ctx_t *ctx, int progress,
                         double speed, double downloaded,
                         double total_size) {
    if (!ctx) return;

    ctx->progress = progress < 0 ? 0 : (progress > 100 ? 100 : progress);
    ctx->speed = speed > 0 ? speed : 0;
    ctx->downloaded = downloaded > 0 ? downloaded : 0;
    ctx->total_size = total_size > 0 ? total_size : 0;

    ctx->elapsed_seconds = (int)(time(NULL) - ctx->start_time);
}

void progress_bar_set_item(progress_ctx_t *ctx, int current_item) {
    if (!ctx) return;
    ctx->current_item = current_item > 0 ? current_item : 1;
    if (ctx->current_item > ctx->total_items) {
        ctx->current_item = ctx->total_items;
    }
}

void progress_bar_render(const progress_ctx_t *ctx) {
    if (!ctx) return;

    /* 构建显示行 */
    char bar_str[128];
    render_progress_bar(ctx, bar_str, sizeof(bar_str));

    char speed_str[32] = "N/A";
    char downloaded_str[32] = "N/A";
    char total_str[32] = "N/A";
    char time_str[32] = "N/A";

    progress_bar_format_time(ctx->elapsed_seconds, time_str, sizeof(time_str));

    /* 速度显示 */
    if (ctx->has_speed && ctx->progress > 0 && ctx->progress < 100) {
        if (ctx->speed > 0) {
            progress_bar_format_size(ctx->speed * 1024 * 1024, speed_str, sizeof(speed_str));
        } else {
            safe_strncpy(speed_str, "N/A", sizeof(speed_str));
        }
    } else if (ctx->progress >= 100) {
        safe_strncpy(speed_str, "N/A", sizeof(speed_str));
    } else {
        safe_strncpy(speed_str, "N/A", sizeof(speed_str));
    }

    /* 下载大小显示 */
    if (ctx->has_speed && ctx->total_size > 0) {
        progress_bar_format_size(ctx->downloaded, downloaded_str, sizeof(downloaded_str));
        progress_bar_format_size(ctx->total_size, total_str, sizeof(total_str));
    } else {
        safe_strncpy(downloaded_str, "N/A", sizeof(downloaded_str));
        safe_strncpy(total_str, "N/A", sizeof(total_str));
    }

    /* 构建标题 */
    char title[128];
    char item_str[32] = "";
    if (ctx->total_items > 1) {
        safe_snprintf(item_str, sizeof(item_str), " (%d/%d)", ctx->current_item, ctx->total_items);
    }

    /* 根据类型选择颜色 */
    const char *color = COLOR_CYAN;
    if (ctx->is_complete) color = COLOR_GREEN;
    if (ctx->has_error) color = COLOR_RED;

    /* 构建完整显示行 */
    char line[512];
    int pos = 0;

    /* 标题 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "%s%s%s: ",
                         color, ctx->name, item_str);
    if (pos >= (int)sizeof(line)) return;

    /* 进度条 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "%s ", bar_str);
    if (pos >= (int)sizeof(line)) return;

    /* 百分比 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "%3d%%", ctx->progress);
    if (pos >= (int)sizeof(line)) return;

    /* 速度 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "  速度: %s", speed_str);
    if (pos >= (int)sizeof(line)) return;

    /* 已下载/总大小 */
    if (ctx->has_speed && ctx->total_size > 0) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  已下载: %s/%s",
                             downloaded_str, total_str);
    } else {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  已下载: N/A");
    }
    if (pos >= (int)sizeof(line)) return;

    /* 耗时 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "  耗时: %s", time_str);
    if (pos >= (int)sizeof(line)) return;

    /* 旋转动画 (仅在运行中且进度 < 100) */
    if (!ctx->is_complete && !ctx->has_error && ctx->progress < 100) {
        const char *spinner = progress_bar_get_spinner_char(ctx->frame_count);
        pos += safe_snprintf(line + pos, sizeof(line) - pos, " %s", spinner);
    }

    line[pos] = '\0';

    /* 输出到终端 (同一行动态刷新) */
    uart_puts("\r");
    uart_puts(line);

    /* 清空行尾残留 */
    int term_width = progress_bar_get_terminal_width();
    int len = strlen(line);
    if (len < term_width) {
        for (int i = len; i < term_width; i++) {
            uart_puts(" ");
        }
    }

    fflush(stdout);
}

void progress_bar_finish(const progress_ctx_t *ctx, int success,
                         const char *message) {
    if (!ctx) return;

    /* 先渲染最终状态 */
    progress_ctx_t final_ctx = *ctx;
    final_ctx.is_complete = 1;
    final_ctx.has_error = success ? 0 : 1;
    final_ctx.progress = 100;

    progress_bar_render(&final_ctx);

    /* 换行 */
    uart_puts("\n");

    /* 显示结果 */
    const char *status_icon = success ? "✅" : "❌";
    const char *status_text = success ? tr("OK", "成功") : tr("FAILED", "失败");
    const char *color = success ? COLOR_GREEN : COLOR_RED;

    uart_puts(color);
    uart_puts(status_icon);
    uart_puts(" ");
    uart_puts(ctx->name);
    uart_puts(" ");
    uart_puts(status_text);

    if (message && message[0]) {
        uart_puts(": ");
        uart_puts(message);
    }

    uart_puts(COLOR_RESET);
    uart_puts("\n");

    LOG_DEBUG_T("ProgressBar", "Finish", "OK", "name='%s', success=%d, message='%s'",
                ctx->name, success, message ? message : "(null)");
}

/* ============================================================
 * 内部辅助：更新帧计数 (在 env_bootstrap 循环中调用)
 * ============================================================ */

void progress_bar_tick(progress_ctx_t *ctx) {
    if (!ctx) return;
    ctx->frame_count++;
    ctx->elapsed_seconds = (int)(time(NULL) - ctx->start_time);
}