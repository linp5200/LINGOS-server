/**
 * @file    src/config/config_renderer_raw.c
 * @brief   RAW 渲染器实现（最终兜底）
 * @version LN-0.4.3
 * @par     在 read_line_uart 循环中添加 feed_heartbeat()
 */

#include "config_renderer_raw.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ============================================================
 * RAW 内部数据
 * ============================================================ */
typedef struct raw_impl_data {
    int initialized;
    int empty_input_count;
} raw_impl_data_t;

/* ============================================================
 * 内部辅助：写入心跳
 * ============================================================ */
static void feed_heartbeat(void) {
    const char *root = lingos_data_root();
    char heartbeat_path[512];
    safe_snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/run/heartbeat", root);
    char run_dir[512];
    safe_snprintf(run_dir, sizeof(run_dir), "%s/run", root);
    mkdir(run_dir, 0755);
    FILE *fp = fopen(heartbeat_path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", time(NULL));
        fclose(fp);
    }
}

/* ============================================================
 * 行输入函数（支持快捷键 + 空输入防错 + 心跳喂狗）
 * ============================================================ */
static int read_line_uart(char *buf, size_t size, int *empty_count) {
    if (!buf || size == 0) return -1;
    int pos = 0;
    int loop_count = 0;

    while (1) {
        loop_count++;
        if (loop_count % 10 == 0) {
            feed_heartbeat();
        }

        char c = uart_getc();

        /* 快捷键 */
        if (c == 0x01) { buf[0]='A'; buf[1]='\0'; return 1; }
        if (c == 0x0C) { buf[0]='L'; buf[1]='\0'; return 2; }
        if (c == 0x12) { buf[0]='R'; buf[1]='\0'; return 3; }
        if (c == 0x11) { buf[0]='Q'; buf[1]='\0'; return 4; }

        if (c == '\r' || c == '\n') {
            if (pos == 0) {
                (*empty_count)++;
                if (*empty_count >= 2 && *empty_count < 5) {
                    uart_puts(tr("\n⚠ No input. Enter twice more to switch mode.\n",
                                 "\n⚠ 未输入。再按两次回车切换模式。\n"));
                }
                if (*empty_count >= 5) {
                    uart_puts(tr("\n⚠ Switching to next mode.\n",
                                 "\n⚠ 正在切换模式。\n"));
                    return -3;
                }
                return 0;
            } else {
                *empty_count = 0;
                buf[pos] = '\0';
                uart_puts("\n");
                return 0;
            }
        } else if (c == 127 || c == 8) {
            /* 【修复】UTF-8 感知退格（RAW 向导） */
            safe_backspace_echo(buf, &pos);
        } else if (c >= 32 && c <= 126) {
            if (pos < (int)size - 1) { buf[pos++] = c; uart_putc(c); }
        }
    }
}

/* ============================================================
 * 自检
 * ============================================================ */
static int raw_self_test(renderer_ctx_t *ctx) {
    (void)ctx;
    LOG_INFO_T("RAW", "SelfTest", "Start", "RAW renderer self-test");
    return 0;
}

static int raw_is_healthy(renderer_ctx_t *ctx) {
    (void)ctx;
    return 1;
}

/* ============================================================
 * 渲染函数（保持不变）
 * ============================================================ */
static int raw_render_header(renderer_ctx_t *ctx, wizard_step_def_t *step, int current, int total) {
    (void)ctx;
    char buf[256];
    safe_snprintf(buf, sizeof(buf), "\n========================================\n");
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), " %s %d/%d: %s\n",
                  tr("Step", "步骤"), current, total,
                  tr(step->title_en, step->title_zh));
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "========================================\n");
    uart_puts(buf);
    return 0;
}

static int raw_render_options(renderer_ctx_t *ctx, wizard_option_t *options, int count, int selected) {
    (void)ctx;
    (void)selected;
    for (int i = 0; i < count; i++) {
        const char *label = tr(options[i].label_en, options[i].label_zh);
        char buf[128];
        if (options[i].is_disabled) {
            safe_snprintf(buf, sizeof(buf), "  [X] %s\n", label);
        } else {
            safe_snprintf(buf, sizeof(buf), "  [%d] %s\n", i + 1, label);
        }
        uart_puts(buf);
    }
    return 0;
}

static int raw_render_input(renderer_ctx_t *ctx, const char *prompt, char *buf, size_t size) {
    (void)ctx;
    (void)buf;
    (void)size;
    if (!prompt) prompt = tr("Enter value:", "输入值：");
    uart_puts(prompt);
    uart_puts(" ");
    return 0;
}

static int raw_render_message(renderer_ctx_t *ctx, const char *msg, int is_error) {
    (void)ctx;
    if (is_error) uart_puts(tr("⚠ ", "⚠ "));
    else uart_puts(tr("✅ ", "✅ "));
    uart_puts(msg);
    uart_puts("\n");
    return 0;
}

static int raw_render_complete(renderer_ctx_t *ctx, int success) {
    (void)ctx;
    if (success) uart_puts(tr("\n✅ Configuration completed!\n", "\n✅ 配置完成！\n"));
    else uart_puts(tr("\n❌ Configuration failed.\n", "\n❌ 配置失败。\n"));
    return 0;
}

static int raw_get_input(renderer_ctx_t *ctx, char *buf, size_t size) {
    (void)ctx;
    raw_impl_data_t *data = (raw_impl_data_t*)ctx->impl;
    uart_puts(tr("> ", "> "));
    return read_line_uart(buf, size, &data->empty_input_count);
}

static int raw_wait_key(renderer_ctx_t *ctx) {
    (void)ctx;
    uart_puts(tr("Press any key to continue...", "按任意键继续..."));
    uart_getc();
    uart_puts("\n");
    return 0;
}

static void raw_cleanup(renderer_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->impl) {
        free(ctx->impl);
        ctx->impl = NULL;
    }
}

/* ============================================================
 * 创建 RAW 渲染器
 * ============================================================ */
int renderer_raw_impl_create(renderer_ctx_t *ctx) {
    if (!ctx) return -1;

    raw_impl_data_t *data = calloc(1, sizeof(raw_impl_data_t));
    if (!data) return -1;

    data->initialized = 1;
    data->empty_input_count = 0;

    ctx->impl = data;
    ctx->render_header = raw_render_header;
    ctx->render_options = raw_render_options;
    ctx->render_input = raw_render_input;
    ctx->render_message = raw_render_message;
    ctx->render_complete = raw_render_complete;
    ctx->get_input = raw_get_input;
    ctx->wait_key = raw_wait_key;
    ctx->cleanup = raw_cleanup;
    ctx->self_test = raw_self_test;
    ctx->is_healthy = raw_is_healthy;

    LOG_INFO_T("RAW", "Create", "OK", "RAW renderer created with heartbeat feed");
    return 0;
}