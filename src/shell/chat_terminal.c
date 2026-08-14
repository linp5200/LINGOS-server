/**
 * @file    chat_terminal.c
 * @brief   专用对话终端（支持思考链和工具调用显示）
 * @version LN-B-5.0.0.0
 * @changes 增加思考链和工具调用显示
 */

#include "chat_terminal.h"
#include "nook.h"
#include "uart.h"
#include "log_extra.h"
#include "lang.h"
#include "safe_string.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ============================================================
 * 显示建议问题（双文）
 * ============================================================ */
static const char *suggestions_en[] = {
    "What is the system status?",
    "List files in current directory",
    "Check network connectivity",
    "Show memory usage",
    "What skills are available?",
    NULL
};
static const char *suggestions_zh[] = {
    "系统状态如何？",
    "帮我列出当前目录的文件",
    "检查网络连通性",
    "显示系统内存使用情况",
    "有哪些可用技能？",
    NULL
};

static void show_suggestions(void) {
    uart_puts(tr(
        "\n\033[1;33m[Suggested Questions]\033[0m\n",
        "\n\033[1;33m[建议问题]\033[0m\n"
    ));
    for (int i = 0; suggestions_en[i]; i++) {
        char buf[128];
        safe_snprintf(buf, sizeof(buf), "  %d. %s\n", i + 1,
                      tr(suggestions_en[i], suggestions_zh[i]));
        uart_puts(buf);
    }
    uart_puts(tr(
        "Enter a number to ask, or type your own message.\n",
        "输入数字直接提问，或输入自定义消息。\n"
    ));
}

/* ============================================================
 * 显示思考链
 * ============================================================ */
static void display_thinking(const char *thinking) {
    if (!thinking || !*thinking) return;

    uart_puts(COLOR_DIM);
    uart_puts(tr(
        "\n┌─ 💭 Thinking ─────────────────────────────────────────────┐\n",
        "\n┌─ 💭 思考中 ──────────────────────────────────────────────┐\n"
    ));
    uart_puts("│ ");
    uart_puts(thinking);
    uart_puts("\n");
    uart_puts(tr(
        "└──────────────────────────────────────────────────────────────┘\n",
        "└──────────────────────────────────────────────────────────────┘\n"
    ));
    uart_puts(COLOR_RESET);
}

/* ============================================================
 * 显示工具调用
 * ============================================================ */
static void display_tool_calls(const char *tool_name, const char *tool_args, const char *tool_result) {
    uart_puts(COLOR_CYAN);
    uart_puts(tr(
        "\n┌─ 🔧 Tool Call ───────────────────────────────────────────┐\n",
        "\n┌─ 🔧 工具调用 ────────────────────────────────────────────┐\n"
    ));
    uart_puts("│  ");
    uart_puts(tr("Tool: ", "工具："));
    uart_puts(tool_name ? tool_name : "unknown");
    uart_puts("\n");
    if (tool_args && *tool_args) {
        uart_puts("│  ");
        uart_puts(tr("Args: ", "参数："));
        uart_puts(tool_args);
        uart_puts("\n");
    }
    if (tool_result && *tool_result) {
        uart_puts("│  ");
        uart_puts(tr("Result: ", "结果："));
        uart_puts(tool_result);
        uart_puts("\n");
    }
    uart_puts(tr(
        "└──────────────────────────────────────────────────────────────┘\n",
        "└──────────────────────────────────────────────────────────────┘\n"
    ));
    uart_puts(COLOR_RESET);
}

/* ============================================================
 * 主对话循环
 * ============================================================ */
void chat_terminal_run(void) {
    uart_puts("\033[2J\033[H");
    log_draw_box(
        tr("Nook Chat Terminal", "Nook 对话终端"),
        tr("Type your message, Ctrl+Q to quit.", "输入消息，Ctrl+Q 退出。"),
        COLOR_CYAN, COLOR_DIM, COLOR_WHITE
    );

    uart_puts("\n");
    show_suggestions();

    char input[4096];
    char response[393216];
    int idx = 0;

    while (1) {
        uart_puts(tr(COLOR_BOLD COLOR_GREEN "> " COLOR_RESET,
                     COLOR_BOLD COLOR_GREEN "> " COLOR_RESET));
        idx = 0;
        while (1) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                input[idx] = '\0';
                break;
            }
            if (c == '\b' || c == 127) {
                /* 【修复】UTF-8 感知退格 */
                safe_backspace_echo(input, &idx);
                continue;
            }
            if (c == 0x11) {
                log_draw_box(
                    tr("Exit", "退出"),
                    tr("Exited chat terminal.", "已退出对话终端。"),
                    COLOR_YELLOW, COLOR_DIM, COLOR_WHITE
                );
                return;
            }
            /* 【修复】终端常规控制键 */
            if (c == 0x03) {   /* ^C：清空当前输入行 */
                uart_puts("\r\033[K");
                uart_puts(tr(COLOR_BOLD COLOR_GREEN "> " COLOR_RESET,
                             COLOR_BOLD COLOR_GREEN "> " COLOR_RESET));
                idx = 0;
                input[0] = '\0';
                continue;
            }
            if (c == 0x15) {   /* ^U：删除整行 */
                while (idx > 0) {
                    safe_backspace_echo(input, &idx);
                }
                continue;
            }
            if (c == 0x0C) {   /* ^L：清屏 */
                uart_puts("\033[2J\033[H");
                continue;
            }
            if (c == 0x01) {   /* ^A：行首（无光标移动，忽略） */
                continue;
            }
            if (idx < (int)sizeof(input) - 1) {
                input[idx++] = c;
                uart_putc(c);
            }
        }

        if (idx == 0) continue;

        /* 检查是否为数字（选择建议问题） */
        if (idx == 1 && input[0] >= '1' && input[0] <= '9') {
            int num = input[0] - '0' - 1;
            if (suggestions_en[num]) {
                const char *selected = tr(suggestions_en[num], suggestions_zh[num]);
                safe_strncpy(input, selected, sizeof(input) - 1);
                input[sizeof(input) - 1] = '\0';
                uart_puts(tr("Selected: ", "已选择："));
                uart_puts(input);
                uart_puts("\n");
            }
        }

        uart_puts(tr(COLOR_DIM "Nook is thinking...\n" COLOR_RESET,
                     COLOR_DIM "Nook 思考中...\n" COLOR_RESET));

        /* 【修复5】流式对话：过程事件 + 逐块实时显示 */
        int ret = nook_ask_stream(input, NULL, response, sizeof(response), 300);

        if (ret == 0) {
            if (strlen(response) == 0) {
                uart_puts(tr(COLOR_RED "No valid response received.\n" COLOR_RESET,
                             COLOR_RED "未收到有效回复。\n" COLOR_RESET));
            }
        } else if (ret == -2) {
            uart_puts(tr(COLOR_RED "Request timed out.\n" COLOR_RESET,
                         COLOR_RED "请求超时。\n" COLOR_RESET));
        } else {
            uart_puts(tr(COLOR_RED "AI service unavailable.\n" COLOR_RESET,
                         COLOR_RED "AI 服务不可用。\n" COLOR_RESET));
        }
        uart_puts("\n");
    }
}