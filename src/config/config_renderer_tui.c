/**
 * @file    src/config/config_renderer_tui.c
 * @brief   TUI 渲染器实现（卡片叠加式配置向导）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 完全重写：
 *          - 卡片叠加显示已完成步骤（步骤标题 + 选中的值）
 *          - 当前步骤在底部交互（选项列表/输入框）
 *          - 方向键 ↑/↓ 支持选项导航
 *          - 多字符输入支持（API Key 等）
 *          - 快捷键 ^A/^L/^R/^Q 支持
 *          - 健康检查和恢复对话框
 */

#include "config_renderer_tui.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../tui/tui_renderer.h"
#include "../tui/tui_defensive.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>

#define TUI_MAX_INPUT 256
#define TUI_MAX_HISTORY 20
#define TUI_MAX_OPTIONS 16

/* ============================================================
 * 步骤历史卡片结构
 * ============================================================ */
typedef struct tui_step_history {
    char title_en[128];
    char title_zh[128];
    char value_en[256];
    char value_zh[256];
} tui_step_history_t;

/* ============================================================
 * TUI 内部数据
 * ============================================================ */
typedef struct tui_impl_data {
    tui_renderer_t renderer;
    int initialized;
    int render_verify_fail_count;
    int force_skip_verify;
    time_t last_render_time;

    /* 卡片叠加相关 */
    tui_step_history_t history[TUI_MAX_HISTORY];
    int history_count;

    /* 当前交互状态 */
    int selected_index;
    int is_input_mode;
    char input_buffer[TUI_MAX_INPUT];
    int input_pos;
    int option_count;

    /* 【新增】当前步骤快照（用于输入回显重绘） */
    wizard_step_def_t *current_step;
    int current_idx;
    int total_steps;
} tui_impl_data_t;

static sigjmp_buf g_tui_jmp;
static int g_tui_failures = 0;

/* ============================================================
 * FTF[信号处理函数，捕获段错误并跳转恢复]
 * ============================================================ */
static void tui_sigsegv_handler(int sig) {
    (void)sig;
    LOG_WARN_T("TUI", "Signal", "Segfault", "caught SIGSEGV");
    siglongjmp(g_tui_jmp, 1);
}

/* ============================================================
 * TUI 可用性检测
 * ============================================================ */
int renderer_tui_available(int force) {
    LOG_DEBUG_T("TUI", "Available", "Enter", "force=%d", force);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        LOG_DEBUG_T("TUI", "Available", "NoTTY", "not a terminal");
        return 0;
    }
    const char *term = getenv("TERM");
    if (!term || strstr(term, "dumb")) {
        LOG_DEBUG_T("TUI", "Available", "BadTerm", "TERM not suitable");
        return 0;
    }
    if (!force) {
        struct notcurses_options opts = {
            .flags = NCOPTION_INHIBIT_SETLOCALE | NCOPTION_SUPPRESS_BANNERS,
            .loglevel = NCLOGLEVEL_FATAL,
        };
        struct notcurses *nc = notcurses_init(&opts, NULL);
        if (!nc) {
            LOG_WARN_T("TUI", "Available", "InitFail", "notcurses_init failed: %s", strerror(errno));
            return 0;
        }
        notcurses_stop(nc);
    }
    LOG_DEBUG_T("TUI", "Available", "OK", "TUI available");
    return 1;
}

/* ============================================================
 * TUI 渲染测试（自检用）
 * ============================================================ */
int renderer_tui_test(char *details, size_t details_size) {
    LOG_DEBUG_T("TUI", "Test", "Enter", "running render test");
    if (!renderer_tui_available(1)) {
        LOG_WARN_T("TUI", "Test", "Unavailable", "TUI not available");
        return -1;
    }

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tui_sigsegv_handler;
    sigaction(SIGSEGV, &sa, &old_sa);
    if (sigsetjmp(g_tui_jmp, 1) != 0) {
        LOG_WARN_T("TUI", "Test", "Crash", "render test crashed");
        sigaction(SIGSEGV, &old_sa, NULL);
        return -1;
    }

    log_set_console_output(0);
    struct notcurses_options opts = {
        .flags = NCOPTION_INHIBIT_SETLOCALE | NCOPTION_SUPPRESS_BANNERS,
        .loglevel = NCLOGLEVEL_FATAL,
    };
    struct notcurses *nc = notcurses_init(&opts, NULL);
    if (!nc) {
        LOG_ERROR_T("TUI", "Test", "InitFail", "notcurses_init failed: %s", strerror(errno));
        log_set_console_output(1);
        sigaction(SIGSEGV, &old_sa, NULL);
        return -1;
    }
    struct ncplane *stdplane = notcurses_stdplane(nc);
    if (!stdplane) {
        notcurses_stop(nc);
        log_set_console_output(1);
        sigaction(SIGSEGV, &old_sa, NULL);
        return -1;
    }
    ncplane_erase(stdplane);
    ncplane_set_fg_rgb(stdplane, 0x00ffff);
    ncplane_putstr(stdplane, "┌─────────────────────────┐\n");
    ncplane_set_fg_rgb(stdplane, 0x00ff00);
    ncplane_putstr(stdplane, "│ TUI Render Test        │\n");
    ncplane_set_fg_rgb(stdplane, 0xffff00);
    ncplane_putstr(stdplane, "│ ◆ ◇ √ × ╞ ╘          │\n");
    ncplane_set_fg_rgb(stdplane, 0xff0000);
    ncplane_putstr(stdplane, "│ [████████████] 100%   │\n");
    ncplane_set_fg_rgb(stdplane, 0xffffff);
    ncplane_putstr(stdplane, "│ ● ○ ✅ ❌ 🔧 💬      │\n");
    ncplane_set_fg_rgb(stdplane, 0x00ffff);
    ncplane_putstr(stdplane, "└─────────────────────────┘\n");
    notcurses_render(nc);
    notcurses_stop(nc);
    log_set_console_output(1);
    sigaction(SIGSEGV, &old_sa, NULL);

    if (details) {
        safe_strncpy(details, "✅ All elements rendered successfully", details_size);
    }
    LOG_DEBUG_T("TUI", "Test", "OK", "render test passed");
    return 0;
}

/* ============================================================
 * FTF[TUI 健康检查]
 * ============================================================ */
static int tui_is_healthy(renderer_ctx_t *ctx) {
    if (!ctx || !ctx->impl) return 0;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;

    if (data->force_skip_verify) return 1;

    if (r->render_fail_count >= 3) {
        LOG_WARN_T("TUI", "Health", "Fail", "render_fail_count=%d", r->render_fail_count);
        return 0;
    }

    time_t now = time(NULL);
    if (now - r->last_render_time > 5) {
        LOG_WARN_T("TUI", "Health", "Stale", "last_render_time=%ld, now=%ld", r->last_render_time, now);
        return 0;
    }

    return 1;
}

/* ============================================================
 * FTF[TUI 自检]
 * ============================================================ */
static int tui_self_test(renderer_ctx_t *ctx) {
    (void)ctx;
    char details[256];
    int ret = renderer_tui_test(details, sizeof(details));
    if (ret == 0) {
        LOG_INFO_T("TUI", "SelfTest", "OK", "%s", details);
    } else {
        LOG_ERROR_T("TUI", "SelfTest", "Fail", "render test failed");
    }
    return ret;
}

/* ============================================================
 * FTF[显示恢复对话框（文本版）]
 * ============================================================ */
static void tui_show_recovery_text(void) {
    uart_puts(COLOR_YELLOW);
    uart_puts("\n╔═══════════════════════════════════════════════════════════╗\n");
    uart_puts("║  ⚠  Rendering anomaly detected                           ║\n");
    uart_puts("║  [^A] Force continue    [^L] Switch to CLI               ║\n");
    uart_puts("║  [^R] Switch to RAW     [^Q] Quit                        ║\n");
    uart_puts("╚═══════════════════════════════════════════════════════════╝\n");
    uart_puts(COLOR_RESET);
}

/* ============================================================
 * FTF[添加历史卡片]
 * ============================================================ */
static void tui_add_history(tui_impl_data_t *data, wizard_step_def_t *step,
                            const char *value_en, const char *value_zh) {
    if (!data || !step) return;
    if (data->history_count >= TUI_MAX_HISTORY) {
        for (int i = 0; i < TUI_MAX_HISTORY - 1; i++) {
            data->history[i] = data->history[i + 1];
        }
        data->history_count = TUI_MAX_HISTORY - 1;
    }
    tui_step_history_t *h = &data->history[data->history_count++];
    safe_strncpy(h->title_en, step->title_en, sizeof(h->title_en));
    safe_strncpy(h->title_zh, step->title_zh, sizeof(h->title_zh));
    if (value_en) safe_strncpy(h->value_en, value_en, sizeof(h->value_en));
    else h->value_en[0] = '\0';
    if (value_zh) safe_strncpy(h->value_zh, value_zh, sizeof(h->value_zh));
    else h->value_zh[0] = '\0';
}

/* ============================================================
 * FTF[渲染主函数：历史卡片 + 当前步骤]
 * ============================================================ */
static void tui_render_full(renderer_ctx_t *ctx, wizard_step_def_t *step,
                            int current, int total) {
    if (!ctx || !ctx->impl || !step) return;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;
    if (!r->main_plane) return;

    ncplane_erase(r->main_plane);
    int row = 0;

    /* ====== 历史卡片 ====== */
    for (int i = 0; i < data->history_count; i++) {
        if (row >= r->main_rows) break;
        tui_step_history_t *h = &data->history[i];
        const char *title = tr(h->title_en, h->title_zh);
        const char *value = tr(h->value_en, h->value_zh);

        ncplane_set_fg_rgb(r->main_plane, 0x00ff00);
        char line[256];
        safe_snprintf(line, sizeof(line), "  %s:", title);
        ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, line);

        if (row >= r->main_rows) break;
        ncplane_set_fg_rgb(r->main_plane, 0xffffff);
        if (value && value[0]) {
            safe_snprintf(line, sizeof(line), "    ✔ %s", value);
        } else {
            safe_snprintf(line, sizeof(line), "    ✔ %s", tr("(selected)", "(已选)"));
        }
        ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, line);
    }

    /* ====== 分隔线 ====== */
    if (data->history_count > 0) {
        ncplane_set_fg_rgb(r->main_plane, 0x444444);
        for (int i = 0; i < r->main_cols; i++) ncplane_putstr(r->main_plane, "─");
        row++;
    }

    /* ====== 当前步骤标题 ====== */
    if (row >= r->main_rows) return;
    const char *title = tr(step->title_en, step->title_zh);
    char buf[256];
    safe_snprintf(buf, sizeof(buf), " %s %d/%d: %s", tr("Step", "步骤"), current, total, title);
    ncplane_set_fg_rgb(r->main_plane, 0x00ffff);
    ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, buf);

    /* ====== 当前步骤内容 ====== */
    if (step->type == STEP_TYPE_SELECT) {
        data->is_input_mode = 0;
        data->option_count = step->option_count;

        if (row < r->main_rows) {
            ncplane_set_fg_rgb(r->main_plane, 0xffffff);
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT,
                                   tr("Select an option:", "选择一个选项:"));
        }

        int selected = data->selected_index;
        if (selected < 0) selected = 0;
        if (selected >= step->option_count) selected = step->option_count - 1;

        for (int i = 0; i < step->option_count; i++) {
            if (row >= r->main_rows) break;
            const char *label = tr(step->options[i].label_en, step->options[i].label_zh);
            if (step->options[i].is_disabled) {
                ncplane_set_fg_rgb(r->main_plane, 0x888888);
                safe_snprintf(buf, sizeof(buf), "    [X] %s", label);
            } else if (i == selected) {
                ncplane_set_fg_rgb(r->main_plane, 0x00ff00);
                safe_snprintf(buf, sizeof(buf), "  > [%d] %s", i + 1, label);
            } else {
                ncplane_set_fg_rgb(r->main_plane, 0xffffff);
                safe_snprintf(buf, sizeof(buf), "    [%d] %s", i + 1, label);
            }
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, buf);
        }

        if (row < r->main_rows) {
            ncplane_set_fg_rgb(r->main_plane, 0x888888);
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT,
                                   tr("↑/↓ navigate | Enter select | ^A force | ^L CLI | ^R RAW | ^Q quit",
                                      "↑/↓ 导航 | Enter 选择 | ^A 强制 | ^L CLI | ^R RAW | ^Q 退出"));
        }
    } else {
        data->is_input_mode = 1;
        const char *prompt = tr(step->input_prompt_en, step->input_prompt_zh);
        if (row < r->main_rows) {
            ncplane_set_fg_rgb(r->main_plane, 0xffffff);
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, prompt);
        }

        if (row < r->main_rows) {
            ncplane_set_fg_rgb(r->main_plane, 0x00ffff);
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT, "> ");
            if (data->input_pos > 0) {
                ncplane_set_fg_rgb(r->main_plane, 0xffffff);
                ncplane_putstr(r->main_plane, data->input_buffer);
            }
            ncplane_set_fg_rgb(r->main_plane, 0xffffff);
            ncplane_putstr(r->main_plane, "_");
        }

        if (row < r->main_rows) {
            ncplane_set_fg_rgb(r->main_plane, 0x888888);
            row++;
            ncplane_putstr_aligned(r->main_plane, row++, NCALIGN_LEFT,
                                   tr("Type input | Enter confirm | ^Q quit",
                                      "输入内容 | Enter 确认 | ^Q 退出"));
        }
    }

    tui_renderer_refresh(r);
    data->last_render_time = time(NULL);
}

/* ============================================================
 * 渲染函数实现（适配 renderer_ctx_t 接口）
 * ============================================================ */
static int tui_render_header(renderer_ctx_t *ctx, wizard_step_def_t *step,
                             int current, int total) {
    /* 保存当前步骤快照（供输入回显重绘使用） */
    if (ctx && ctx->impl) {
        tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
        data->current_step = step;
        data->current_idx = current;
        data->total_steps = total;
    }
    /* 核心渲染：完整绘制历史卡片 + 当前步骤（此前为空实现导致界面空白） */
    tui_render_full(ctx, step, current, total);
    return 0;
}

static int tui_render_options(renderer_ctx_t *ctx, wizard_option_t *options,
                              int count, int selected) {
    if (ctx && ctx->impl) {
        tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
        if (selected >= 0 && selected < count) {
            data->selected_index = selected;
        }
        data->option_count = count;
    }
    (void)options;
    return 0;
}

static int tui_render_input(renderer_ctx_t *ctx, const char *prompt,
                            char *buf, size_t size) {
    /* 实际渲染由 tui_render_full 完成 */
    (void)ctx; (void)prompt; (void)buf; (void)size;
    return 0;
}

static int tui_render_message(renderer_ctx_t *ctx, const char *msg, int is_error) {
    if (!ctx || !ctx->impl) return -1;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;
    if (!r->main_plane) return -1;

    ncplane_erase(r->main_plane);
    ncplane_set_fg_rgb(r->main_plane, is_error ? 0xff0000 : 0x00ff00);
    ncplane_putstr_aligned(r->main_plane, 0, NCALIGN_CENTER, msg);
    tui_renderer_refresh(r);
    return 0;
}

static int tui_render_complete(renderer_ctx_t *ctx, int success) {
    if (!ctx || !ctx->impl) return -1;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;
    if (!r->main_plane) return -1;

    ncplane_erase(r->main_plane);
    const char *msg = success ? tr("✅ Configuration completed successfully!",
                                   "✅ 配置完成！") :
                                tr("❌ Configuration failed.",
                                   "❌ 配置失败。");
    ncplane_set_fg_rgb(r->main_plane, success ? 0x00ff00 : 0xff0000);
    ncplane_putstr_aligned(r->main_plane, 0, NCALIGN_CENTER, msg);
    ncplane_set_fg_rgb(r->main_plane, 0x888888);
    ncplane_putstr_aligned(r->main_plane, 2, NCALIGN_CENTER,
                           tr("Press any key to continue...", "按任意键继续..."));
    tui_renderer_refresh(r);
    return 0;
}

/* ============================================================
 * FTF[获取用户输入（阻塞，支持方向键、多字符、快捷键）]
 * ============================================================ */
static int tui_get_input(renderer_ctx_t *ctx, char *buf, size_t size) {
    if (!ctx || !ctx->impl || !buf || size == 0) return -1;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;
    if (!r->nc) return -1;

    /* 健康检查 */
    if (!tui_is_healthy(ctx) && !data->force_skip_verify) {
        tui_show_recovery_text();
        return -2;
    }

    /* ====== 输入模式：多字符输入 ====== */
    if (data->is_input_mode) {
        while (1) {
            int key = tui_renderer_get_key(r, -1);
            if (key == -1) {
                LOG_ERROR_T("TUI", "GetInput", "KeyError", "tui_renderer_get_key returned -1");
                return -1;
            }

            /* 【修复】^C 在 TUI 中作为无害按键忽略（不触发退出/信号） */
            if (key == 0x03) continue;

            if (key == 0x01) { data->force_skip_verify = 1; buf[0]='A'; buf[1]='\0'; return 1; }
            if (key == 0x0C) { buf[0]='L'; buf[1]='\0'; return 2; }
            if (key == 0x12) { buf[0]='R'; buf[1]='\0'; return 3; }
            if (key == 0x11) { buf[0]='Q'; buf[1]='\0'; return 4; }

            if (key == 10 || key == 13) {
                data->input_buffer[data->input_pos] = '\0';
                safe_strncpy(buf, data->input_buffer, size);
                data->input_pos = 0;
                data->input_buffer[0] = '\0';
                return 0;
            } else if (key == 127 || key == 8) {
                if (data->input_pos > 0) {
                    data->input_pos--;
                    data->input_buffer[data->input_pos] = '\0';
                    /* 【新增】输入回显刷新 */
                    if (data->current_step) {
                        tui_render_full(ctx, data->current_step, data->current_idx, data->total_steps);
                    }
                }
            } else if (key >= 32 && key <= 126) {
                if (data->input_pos < TUI_MAX_INPUT - 1) {
                    data->input_buffer[data->input_pos++] = (char)key;
                    data->input_buffer[data->input_pos] = '\0';
                    /* 【新增】输入回显刷新 */
                    if (data->current_step) {
                        tui_render_full(ctx, data->current_step, data->current_idx, data->total_steps);
                    }
                }
            }
        }
    }

    /* ====== 选择模式：方向键 + 数字键 ====== */
    while (1) {
        int key = tui_renderer_get_key(r, -1);
        if (key == -1) {
            LOG_ERROR_T("TUI", "GetInput", "KeyError", "tui_renderer_get_key returned -1");
            return -1;
        }

        /* 【修复】^C 在 TUI 中作为无害按键忽略（不触发退出/信号） */
        if (key == 0x03) continue;

        /* 快捷键 */
        if (key == 0x01) { data->force_skip_verify = 1; buf[0]='A'; buf[1]='\0'; return 1; }
        if (key == 0x0C) { buf[0]='L'; buf[1]='\0'; return 2; }
        if (key == 0x12) { buf[0]='R'; buf[1]='\0'; return 3; }
        if (key == 0x11) { buf[0]='Q'; buf[1]='\0'; return 4; }

        /* 方向键（由 tui_renderer_get_key 映射：UP=1000, DOWN=1001） */
        if (key == 1000) { /* UP */
            if (data->selected_index > 0) data->selected_index--;
            return 0;
        }
        if (key == 1001) { /* DOWN */
            if (data->selected_index < data->option_count - 1) data->selected_index++;
            return 0;
        }

        /* 【修复】Enter 确认当前选中项（此前未处理导致无法回车） */
        if (key == 10 || key == 13) {
            if (data->selected_index >= 0 && data->selected_index < data->option_count) {
                buf[0] = (char)('1' + data->selected_index);
                buf[1] = '\0';
                return 0;
            }
        }

        /* 数字键（1-9） */
        if (key >= '1' && key <= '9') {
            int choice = key - '0';
            if (choice <= data->option_count) {
                data->selected_index = choice - 1;
                buf[0] = (char)key;
                buf[1] = '\0';
                return 0;
            }
        }

        /* 'q' 或 ESC 退出 */
        if (key == 'q' || key == 'Q' || key == 27) {
            buf[0] = 'q';
            buf[1] = '\0';
            return 0;
        }
    }
}

static int tui_wait_key(renderer_ctx_t *ctx) {
    if (!ctx || !ctx->impl) return -1;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    tui_renderer_t *r = &data->renderer;
    if (!r->nc) return -1;
    tui_renderer_get_key(r, -1);
    return 0;
}

static void tui_cleanup(renderer_ctx_t *ctx) {
    if (!ctx) return;
    tui_impl_data_t *data = (tui_impl_data_t*)ctx->impl;
    if (data && data->initialized) {
        tui_renderer_destroy(&data->renderer);
        data->initialized = 0;
    }
}

/* ============================================================
 * FTF[创建 TUI 渲染器]
 * ============================================================ */
int renderer_tui_impl_create(renderer_ctx_t *ctx) {
    if (!ctx) return -1;

    tui_impl_data_t *data = calloc(1, sizeof(tui_impl_data_t));
    if (!data) return -1;

    /* FF[src/tui/tui_renderer.c]-CFN[tui_renderer_init]-FTF[初始化 Notcurses 渲染器] */
    if (tui_renderer_init(&data->renderer) != 0) {
        free(data);
        LOG_ERROR_T("TUI", "Create", "InitFail", "tui_renderer_init failed");
        return -1;
    }

    data->initialized = 1;
    data->selected_index = 0;
    data->is_input_mode = 0;
    data->input_pos = 0;
    data->input_buffer[0] = '\0';
    data->force_skip_verify = 0;
    data->last_render_time = time(NULL);
    data->history_count = 0;
    data->option_count = 0;

    ctx->impl = data;
    ctx->render_header = tui_render_header;
    ctx->render_options = tui_render_options;
    ctx->render_input = tui_render_input;
    ctx->render_message = tui_render_message;
    ctx->render_complete = tui_render_complete;
    ctx->get_input = tui_get_input;
    ctx->wait_key = tui_wait_key;
    ctx->cleanup = tui_cleanup;
    ctx->self_test = tui_self_test;
    ctx->is_healthy = tui_is_healthy;

    LOG_INFO_T("TUI", "Create", "OK", "TUI renderer created (card-based)");
    return 0;
}