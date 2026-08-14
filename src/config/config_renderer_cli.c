/**
 * @file    src/config/config_renderer_cli.c
 * @brief   CLI 渲染器实现（完整版：健康检查 + 快捷键 + 空输入防错 + 心跳喂狗）
 * @version LN-B-5.1.2.6-rc
 * @par     在 read_line_uart 循环中添加 feed_heartbeat()，防止监督者超时重启。
 */

#include "config_renderer_cli.h"
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
#include <ctype.h>
#include <time.h>

/* ============================================================
 * CLI 内部数据
 * ============================================================ */
typedef struct cli_impl_data {
    int initialized;
    int empty_input_count;   // 空输入计数器
} cli_impl_data_t;

/* ============================================================
 * 内部辅助：写入心跳（喂狗）
 * ============================================================ */
static void feed_heartbeat(void) {
    const char *root = lingos_data_root();
    char heartbeat_path[512];
    safe_snprintf(heartbeat_path, sizeof(heartbeat_path), "%s/run/heartbeat", root);

    /* 确保 run 目录存在 */
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
        // 每 10 次循环写一次心跳（大约每 10 个字符或 10 次按键）
        loop_count++;
        if (loop_count % 10 == 0) {
            feed_heartbeat();
        }

        char c = uart_getc();

        /* ---- 快捷键检测（返回特殊值） ---- */
        if (c == 0x01) { // ^A: 强制继续
            buf[0] = 'A';
            buf[1] = '\0';
            return 1;
        }
        if (c == 0x0C) { // ^L: 切换到 CLI（如果当前不是 CLI）
            buf[0] = 'L';
            buf[1] = '\0';
            return 2;
        }
        if (c == 0x12) { // ^R: 切换到 RAW
            buf[0] = 'R';
            buf[1] = '\0';
            return 3;
        }
        if (c == 0x11) { // ^Q: 退出
            buf[0] = 'Q';
            buf[1] = '\0';
            return 4;
        }

        /* ---- 正常输入处理 ---- */
        if (c == '\r' || c == '\n') {
            /* 回车：检查是否为空输入 */
            if (pos == 0) {
                (*empty_count)++;
                if (*empty_count >= 2 && *empty_count < 5) {
                    uart_puts(tr("\n⚠ No input detected. Enter twice more to switch to a more reliable mode.\n",
                                 "\n⚠ 未检测到输入。再按两次回车将切换至更可靠的模式。\n"));
                }
                if (*empty_count >= 5) {
                    uart_puts(tr("\n⚠ Multiple empty inputs. Switching to next reliable mode.\n",
                                 "\n⚠ 多次空输入。正在切换至更可靠的模式。\n"));
                    buf[0] = '\0';
                    return -3;  // 触发自动降级
                }
                buf[0] = '\0';
                return 0;  // 空输入，继续提示
            } else {
                /* 非空输入，重置计数器 */
                *empty_count = 0;
                buf[pos] = '\0';
                uart_puts("\n");
                return 0;
            }
        } else if (c == 127 || c == 8) {
            /* 退格（【修复】UTF-8 感知，中文/全角按宽度擦除） */
            safe_backspace_echo(buf, &pos);
        } else if (c >= 32 && c <= 126) {
            /* 可打印字符 */
            if (pos < (int)size - 1) {
                buf[pos++] = c;
                uart_putc(c);
            }
        }
    }
}

/* ============================================================
 * 自检函数
 * ============================================================ */
static int cli_self_test(renderer_ctx_t *ctx) {
    (void)ctx;
    LOG_INFO_T("CLI", "SelfTest", "Start", "CLI renderer self-test");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        LOG_WARN_T("CLI", "SelfTest", "NoTTY", "not a terminal");
        return -1;
    }
    LOG_INFO_T("CLI", "SelfTest", "OK", "CLI renderer is healthy");
    return 0;
}

/* ============================================================
 * 健康检查
 * ============================================================ */
static int cli_is_healthy(renderer_ctx_t *ctx) {
    (void)ctx;
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

/* ============================================================
 * 渲染函数（保持不变）
 * ============================================================ */
static int cli_render_header(renderer_ctx_t *ctx, wizard_step_def_t *step, int current, int total) {
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

static int cli_render_options(renderer_ctx_t *ctx, wizard_option_t *options, int count, int selected) {
    (void)ctx;
    (void)selected;
    for (int i = 0; i < count; i++) {
        const char *label = tr(options[i].label_en, options[i].label_zh);
        char buf[256];
        if (options[i].is_disabled) {
            safe_snprintf(buf, sizeof(buf), "  %d. [X] %s\n", i + 1, label);
        } else {
            safe_snprintf(buf, sizeof(buf), "  %d. [ ] %s\n", i + 1, label);
        }
        uart_puts(buf);
    }
    return 0;
}

static int cli_render_input(renderer_ctx_t *ctx, const char *prompt, char *buf, size_t size) {
    (void)ctx;
    (void)buf;
    (void)size;
    if (!prompt) prompt = tr("Enter value:", "输入值：");
    uart_puts(prompt);
    uart_puts(" ");
    return 0;
}

static int cli_render_message(renderer_ctx_t *ctx, const char *msg, int is_error) {
    (void)ctx;
    const char *color = is_error ? COLOR_RED : COLOR_GREEN;
    uart_puts(color);
    uart_puts(msg);
    uart_puts(COLOR_RESET);
    uart_puts("\n");
    return 0;
}

static int cli_render_complete(renderer_ctx_t *ctx, int success) {
    (void)ctx;
    if (success) {
        uart_puts(COLOR_GREEN);
        uart_puts(tr("✅ Configuration completed successfully!\n", "✅ 配置完成！\n"));
        uart_puts(COLOR_RESET);
    } else {
        uart_puts(COLOR_RED);
        uart_puts(tr("❌ Configuration failed.\n", "❌ 配置失败。\n"));
        uart_puts(COLOR_RESET);
    }
    return 0;
}

static int cli_get_input(renderer_ctx_t *ctx, char *buf, size_t size) {
    (void)ctx;
    cli_impl_data_t *data = (cli_impl_data_t*)ctx->impl;
    uart_puts(tr("Enter choice (1-9), 'q' to cancel: ", "输入选项 (1-9)，'q' 取消："));
    return read_line_uart(buf, size, &data->empty_input_count);
}

static int cli_wait_key(renderer_ctx_t *ctx) {
    (void)ctx;
    uart_puts(tr("Press any key to continue...", "按任意键继续..."));
    uart_getc();
    uart_puts("\n");
    return 0;
}

static void cli_cleanup(renderer_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->impl) {
        free(ctx->impl);
        ctx->impl = NULL;
    }
}

/* ============================================================
 * 创建 CLI 渲染器
 * ============================================================ */
int renderer_cli_impl_create(renderer_ctx_t *ctx) {
    if (!ctx) return -1;

    cli_impl_data_t *data = calloc(1, sizeof(cli_impl_data_t));
    if (!data) return -1;

    data->initialized = 1;
    data->empty_input_count = 0;

    ctx->impl = data;
    ctx->render_header = cli_render_header;
    ctx->render_options = cli_render_options;
    ctx->render_input = cli_render_input;
    ctx->render_message = cli_render_message;
    ctx->render_complete = cli_render_complete;
    ctx->get_input = cli_get_input;
    ctx->wait_key = cli_wait_key;
    ctx->cleanup = cli_cleanup;
    ctx->self_test = cli_self_test;
    ctx->is_healthy = cli_is_healthy;

    LOG_INFO_T("CLI", "Create", "OK", "CLI renderer created with heartbeat feed");
    return 0;
}