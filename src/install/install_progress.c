/**
 * @file    src/install/install_progress.c
 * @brief   安装进度显示实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "install_progress.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static char g_title[128] = {0};
static char g_stage[64] = {0};
static char g_item[64] = {0};
static int g_stage_current = 0;
static int g_stage_total = 0;
static int g_item_current = 0;
static int g_item_total = 0;
static int g_last_progress = -1;
static time_t g_start_time = 0;
static int g_item_success = 0;
static int g_item_failed = 0;

/* ============================================================
 * 进度条渲染（单行动态刷新 + 彩色显眼）
 * ============================================================ */
static void render_progress_line(int progress, double speed,
                                 double downloaded, double total_size,
                                 const char *label) {
    char buf[128];
    uart_puts("\r\033[K");

    /* 信息段（青色） */
    uart_puts(COLOR_CYAN);
    uart_puts(g_title);
    if (g_stage_total > 0) {
        safe_snprintf(buf, sizeof(buf), "  %s (%d/%d)", g_stage, g_stage_current, g_stage_total);
        uart_puts(buf);
    }
    if (g_item_total > 0) {
        safe_snprintf(buf, sizeof(buf), "  %s (%d/%d)", g_item, g_item_current, g_item_total);
        uart_puts(buf);
    }
    if (label && label[0]) {
        safe_snprintf(buf, sizeof(buf), "  %s", label);
        uart_puts(buf);
    }
    uart_puts(COLOR_RESET);

    /* 进度条（绿色填充，显眼） */
    int bar_width = 50;
    int filled = (progress * bar_width) / 100;
    uart_puts(" [");
    uart_puts(COLOR_GREEN);
    for (int i = 0; i < filled; i++) uart_puts("█");
    uart_puts(COLOR_RESET);
    for (int i = filled; i < bar_width; i++) uart_puts(" ");
    uart_puts("] ");

    /* 百分比（黄色） */
    safe_snprintf(buf, sizeof(buf), "%3d%%", progress);
    uart_puts(COLOR_YELLOW);
    uart_puts(buf);
    uart_puts(COLOR_RESET);

    /* 速度 / 大小 / 耗时 */
    if (speed > 0) {
        safe_snprintf(buf, sizeof(buf), "  速度: %.1fMB/s", speed);
        uart_puts(buf);
    }
    if (total_size > 0) {
        safe_snprintf(buf, sizeof(buf), "  已下载: %.1f/%.1fMB", downloaded, total_size);
        uart_puts(buf);
    }
    time_t now = time(NULL);
    int elapsed = (int)(now - g_start_time);
    if (elapsed > 0) {
        safe_snprintf(buf, sizeof(buf), "  耗时: %ds", elapsed);
        uart_puts(buf);
    }
    fflush(stdout);
}

/* ============================================================
 * 初始化进度（醒目横幅，与市面安装显示一致）
 * ============================================================ */
void install_progress_init(const char *title) {
    if (title) {
        safe_strncpy(g_title, title, sizeof(g_title));
    }
    g_start_time = time(NULL);
    g_last_progress = -1;
    g_item_success = 0;
    g_item_failed = 0;

    /* 【优化】安装横幅（市面风格） */
    uart_puts("\n");
    uart_puts(COLOR_CYAN);
    uart_puts("╔══════════════════════════════════════════════════════════╗\n");
    uart_puts("║");
    uart_puts(COLOR_BOLD);
    uart_puts("         LING OS 安装助手  /  LING OS Setup          ");
    uart_puts(COLOR_CYAN);
    uart_puts("║\n");
    uart_puts("║");
    uart_puts(COLOR_RESET);
    uart_puts(COLOR_WHITE);
    char sub[64];
    safe_snprintf(sub, sizeof(sub), "  %-50s", g_title);
    uart_puts(sub);
    uart_puts(COLOR_CYAN);
    uart_puts("║\n");
    uart_puts("╚══════════════════════════════════════════════════════════╝\n");
    uart_puts(COLOR_RESET);
    uart_puts("\n");
}

/* ============================================================
 * 设置阶段（阶段横幅，醒目指示）
 * ============================================================ */
void install_progress_set_stage(const char *stage_name, int current, int total) {
    if (stage_name) {
        safe_strncpy(g_stage, stage_name, sizeof(g_stage));
    }
    g_stage_current = current;
    g_stage_total = total;
    g_item_current = 0;
    g_item_total = 0;

    /* 【优化】阶段横幅（黄色，市面风格） */
    uart_puts(COLOR_YELLOW);
    char buf[96];
    safe_snprintf(buf, sizeof(buf), "\n  ── %s  (%d/%d) ──\n",
                  g_stage[0] ? g_stage : "Stage", current, total);
    uart_puts(buf);
    uart_puts(COLOR_RESET);
}

/* ============================================================
 * 设置项目
 * ============================================================ */
void install_progress_set_item(const char *item_name, int current, int total) {
    if (item_name) {
        safe_strncpy(g_item, item_name, sizeof(g_item));
    }
    g_item_current = current;
    g_item_total = total;
    g_start_time = time(NULL);
}

/* ============================================================
 * 更新进度
 * ============================================================ */
void install_progress_update(int progress, double speed, double downloaded,
                             double total_size, const char *label) {
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    if (progress == g_last_progress && speed == 0) return;
    g_last_progress = progress;
    render_progress_line(progress, speed, downloaded, total_size, label);
}

/* ============================================================
 * 完成项目
 * ============================================================ */
void install_progress_finish_item(int success) {
    const char *icon = success ? "✅" : "❌";
    const char *status = success ? tr("OK", "成功") : tr("FAILED", "失败");
    uart_puts("\r\033[K");
    uart_puts(icon);
    uart_puts(" ");
    uart_puts(g_item);
    uart_puts(" ");
    uart_puts(status);
    uart_puts("\n");
    if (success) g_item_success++;
    else g_item_failed++;
    g_last_progress = -1;
}

/* ============================================================
 * 完成安装
 * ============================================================ */
void install_progress_finish(const install_summary_t *summary) {
    if (!summary) return;
    uart_puts("\n");
    uart_puts(tr("═══════ Installation Summary ═══════\n", "═══════ 安装汇总 ═══════\n"));
    char buf[128];
    safe_snprintf(buf, sizeof(buf), tr("Total: %d packages\n", "总计：%d 个包\n"),
                  summary->total);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("  ✅ Succeeded: %d\n", "  ✅ 成功：%d\n"),
                  summary->success_count);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("  ❌ Failed: %d\n", "  ❌ 失败：%d\n"),
                  summary->failed_count);
    uart_puts(buf);

    if (summary->failed_count > 0) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("\n⚠ Some packages failed to install.\n",
                     "\n⚠ 部分包安装失败。\n"));
        uart_puts(tr("  LING OS will start in limited mode.\n",
                     "  LING OS 将以受限模式启动。\n"));
        uart_puts(tr("  Please check logs for details.\n",
                     "  请查看日志了解详情。\n"));
        uart_puts(COLOR_RESET);
    } else {
        uart_puts(COLOR_GREEN);
        uart_puts(tr("\n✅ All packages installed successfully!\n",
                     "\n✅ 所有包安装成功！\n"));
        uart_puts(COLOR_RESET);
    }
    uart_puts(tr("Press any key to continue...\n", "按任意键继续...\n"));
    uart_getc();
    uart_puts("\n");
}