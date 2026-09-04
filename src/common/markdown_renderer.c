/**
 * @file    src/common/markdown_renderer.c
 * @brief   Markdown → ANSI 富文本渲染（终端）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 * @par     支持：**粗体** *斜体* `代码` | 表格 | --- 分隔线 - 列表 ```代码块```
 *          链接/图片不渲染（终端环境，按需求确认）
 */

#include "markdown_renderer.h"
#include "safe_string.h"
#include "lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <string.h>
#include <ctype.h>

#define MD_BOLD     "\033[1m"
#define MD_ITALIC   "\033[3m"
#define MD_CODE     "\033[36m"
#define MD_TABLE    "\033[35m"
#define MD_RESET    "\033[0m"

/* ============================================================
 * 内联渲染：**粗体** *斜体* `代码`
 * ============================================================ */
static void md_render_inline(const char *s) {
    if (!s) return;
    char buf[512];
    while (*s) {
        /* 粗体 ** */
        if (strncmp(s, "**", 2) == 0) {
            const char *end = strstr(s + 2, "**");
            if (end) {
                size_t n = (size_t)(end - (s + 2));
                if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                memcpy(buf, s + 2, n);
                buf[n] = '\0';
                uart_puts(MD_BOLD);
                uart_puts(buf);
                uart_puts(MD_RESET);
                s = end + 2;
                continue;
            }
        }
        /* 斜体 * （排除 ** 和行尾） */
        if (*s == '*' && strncmp(s, "**", 2) != 0) {
            const char *end = strchr(s + 1, '*');
            if (end && *(end + 1) != '*') {
                size_t n = (size_t)(end - (s + 1));
                if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                memcpy(buf, s + 1, n);
                buf[n] = '\0';
                uart_puts(MD_ITALIC);
                uart_puts(buf);
                uart_puts(MD_RESET);
                s = end + 1;
                continue;
            }
        }
        /* 行内代码 ` */
        if (*s == '`') {
            const char *end = strchr(s + 1, '`');
            if (end) {
                size_t n = (size_t)(end - (s + 1));
                if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                memcpy(buf, s + 1, n);
                buf[n] = '\0';
                uart_puts(MD_CODE);
                uart_puts(buf);
                uart_puts(MD_RESET);
                s = end + 1;
                continue;
            }
        }
        uart_putc(*s);
        s++;
    }
}

/* ============================================================
 * 行类型识别
 * ============================================================ */
int md_is_table_row(const char *line) {
    if (!line) return 0;
    return strchr(line, '|') != NULL;
}

static int md_is_separator(const char *line) {
    if (!line || !*line) return 0;
    /* --- 或 === 或 |---|---| */
    if (strncmp(line, "---", 3) == 0 || strncmp(line, "===", 3) == 0) {
        return 1;
    }
    if (line[0] == '|' && strstr(line, "---")) return 1;
    return 0;
}

static int md_is_list_item(const char *line) {
    if (!line) return 0;
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '-' && *(p + 1) == ' ') return 1;
    if (*p == '*' && *(p + 1) == ' ') return 1;
    if (isdigit((unsigned char)*p)) {
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == '.' && *(q + 1) == ' ') return 1;
    }
    return 0;
}

/* ============================================================
 * 渲染单行
 * ============================================================ */
void md_render_line(const char *line) {
    if (!line) return;

    /* 分隔线 */
    if (md_is_separator(line)) {
        uart_puts(COLOR_YELLOW);
        uart_puts("────────────────────────────────────────────────────────\n");
        uart_puts(COLOR_RESET);
        return;
    }

    /* 表格行（青色/紫色边框样式） */
    if (md_is_table_row(line)) {
        uart_puts(MD_TABLE);
        /* 替换 | 为 │（视觉分隔） */
        char buf[512];
        size_t n = strlen(line);
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        for (size_t i = 0; i < n; i++) {
            buf[i] = (line[i] == '|') ? '│' : line[i];
        }
        buf[n] = '\0';
        uart_puts(buf);
        uart_puts(MD_RESET);
        uart_puts("\n");
        return;
    }

    /* 列表项（缩进 + 符号） */
    if (md_is_list_item(line)) {
        uart_puts(COLOR_GREEN);
        uart_puts("  ");
        uart_puts(COLOR_RESET);
        md_render_inline(line);
        uart_puts("\n");
        return;
    }

    /* 普通行（内联渲染） */
    md_render_inline(line);
    uart_puts("\n");
}

/* ============================================================
 * 流式块渲染（内联粗体/斜体/代码，容忍跨块截断）
 * ============================================================ */
void md_render_stream_delta(const char *delta) {
    if (!delta) return;
    md_render_inline(delta);
}

/* ============================================================
 * 流式行渲染状态（逐行识别表格/列表/分隔线）
 * ============================================================ */
static char g_stream_line[1024];
static size_t g_stream_len = 0;

void md_stream_feed(const char *text) {
    if (!text) return;
    while (*text) {
        if (*text == '\n') {
            g_stream_line[g_stream_len] = '\0';
            g_stream_len = 0;
            if (g_stream_line[0]) {
                md_render_line(g_stream_line);
            } else {
                uart_puts("\n");
            }
        } else if (g_stream_len < sizeof(g_stream_line) - 1) {
            g_stream_line[g_stream_len++] = *text;
        }
        text++;
    }
}

void md_stream_flush(void) {
    if (g_stream_len > 0) {
        g_stream_line[g_stream_len] = '\0';
        g_stream_len = 0;
        md_render_line(g_stream_line);
    }
}

/* ============================================================
 * 渲染多行（含代码块状态）
 * ============================================================ */
void md_render_text(const char *text) {
    if (!text) return;
    char line[1024];
    int in_code_block = 0;
    size_t pos = 0;

    while (*text) {
        char c = *text++;
        if (c == '\n' || c == '\0') {
            line[pos] = '\0';
            pos = 0;

            /* 代码块开关 ``` */
            if (strncmp(line, "```", 3) == 0) {
                if (in_code_block) {
                    uart_puts(MD_RESET);
                    uart_puts("\n");
                    in_code_block = 0;
                } else {
                    uart_puts(MD_CODE);
                    uart_puts("\n");
                    in_code_block = 1;
                }
                if (c == '\0') break;
                continue;
            }

            if (in_code_block) {
                uart_puts(line);
                uart_puts("\n");
            } else {
                md_render_line(line);
            }
            if (c == '\0') break;
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = c;
        }
    }
    if (pos > 0) {
        line[pos] = '\0';
        md_render_line(line);
    }
    if (in_code_block) uart_puts(MD_RESET);
}
