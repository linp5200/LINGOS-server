/**
 * @file    src/config/config_renderer_clack.c
 * @brief   Clack 风格配置渲染器（先生 2026-10-01 方案 A 定稿）
 * @version LN-0.8.2
 * @par     设计要点（先生定稿逐条对应）：
 *          1. 符号体系：◆ 当前块 / │ 引导线 / └ 块结束 / √ 完成 / ▲ 错误 / ● ○ 选项
 *          2. 分隔符：# = 未确认块；## = 已确认块（绿色）——分隔符非层级
 *          3. 无步骤编号、无"还剩几步"计数
 *          4. 动画：打字机 / 呼吸光标 / 保存进度 spinner
 *          5. 全量重绘（清块重画，不刷屏）
 *          6. 方向键 ↑/↓ 移动；^A/^L/^R/^Q 与旧渲染器同语义；ESC = 返回
 *          7. 旧渲染器（TUI/CLI/RAW）零改动，保留回退
 *          8. 声明式清单 16 字段中与渲染相关的项全部呈现：
 *             group/desc双/advanced/skippable/secret/restart_required/default
 * @note    本地纪律：不编译（禁 make / 禁链接）——仅 gcc -fsyntax-only 静态审核。
 * @par     核心协议：C1, C-C
 */

#include "config_renderer_clack.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ============================================================
 * 常量与符号
 * ============================================================ */
#define CLACK_MAX_OPTIONS    16
#define CLACK_MAX_CONFIRMED  32

#define CLACK_TYPE_US        38000   /* 打字机每字符（微秒） */
#define CLACK_SPIN_US        90000   /* spinner 每帧（微秒） */
#define CLACK_BREATH_US      55000   /* 呼吸光标每帧（微秒） */

#define CLACK_ACTIVE   "◆"   /* 当前块 */
#define CLACK_BAR      "│"   /* 引导线 */
#define CLACK_END      "└"   /* 块结束 */
#define CLACK_OK       "√"   /* 完成 */
#define CLACK_ERR      "▲"   /* 错误 */
#define CLACK_DOT      "●"   /* 选项：选中 */
#define CLACK_RING     "○"   /* 选项：未选 */
#define CLACK_PEND     "◇"   /* 待确认块标记 */

static const char *const CLACK_SPIN_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
#define CLACK_SPIN_COUNT ((int)(sizeof(CLACK_SPIN_FRAMES) / sizeof(CLACK_SPIN_FRAMES[0])))

/* 按键分类 */
#define CLACK_KEY_EOF    0
#define CLACK_KEY_CHAR   1
#define CLACK_KEY_UP     2
#define CLACK_KEY_DOWN   3
#define CLACK_KEY_LEFT   4
#define CLACK_KEY_RIGHT  5

/* ============================================================
 * 内部数据
 * ============================================================ */
typedef struct clack_confirmed {
    char title_en[64];
    char title_zh[64];
} clack_confirmed_t;

typedef struct clack_impl_data {
    int initialized;
    int is_tty;
    int anim;                    /* 动画开关（非 tty 自动关闭） */
    int lines_drawn;             /* 上次绘制行数（全量重绘基准） */

    /* 当前步骤快照（声明式清单字段） */
    char cur_step_id[32];
    char cur_title_en[64];
    char cur_title_zh[64];
    char cur_desc_en[128];
    char cur_desc_zh[128];
    char cur_default[64];
    int  cur_advanced;
    int  cur_skippable;
    int  cur_secret;
    int  cur_restart;

    /* 选项快照 */
    wizard_option_t opts[CLACK_MAX_OPTIONS];
    int opt_count;
    int opt_sel;
    int opt_active;
    int navigated;
    int has_default;
    int head_animated;           /* 本步标题打字机只播一次 */

    /* 已确认块历史（## 绿色） */
    clack_confirmed_t confirmed[CLACK_MAX_CONFIRMED];
    int confirmed_count;

    int empty_input_count;
    int raw_active;
} clack_impl_data_t;

/* 终端原始模式全局保存（信号退出兜底） */
static struct termios g_clack_saved_term;
static int g_clack_term_saved = 0;
static int g_clack_atexit_done = 0;

/* ============================================================
 * 基础工具
 * ============================================================ */
static void clack_sleep_us(long us) {
    struct timespec ts;
    if (us <= 0) return;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) break;
    }
}

static void clack_feed_heartbeat(void) {
    const char *root = lingos_data_root();
    char path[512];
    char dir[512];
    FILE *fp;

    if (!root) return;
    safe_snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0755);
    safe_snprintf(path, sizeof(path), "%s/run/heartbeat", root);
    fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", (long)time(NULL));
        fclose(fp);
    }
}

static void clack_puts_line(clack_impl_data_t *d, const char *s) {
    const char *p;
    if (!s) return;
    uart_puts(s);
    if (!d || !d->is_tty) return;
    for (p = s; *p; p++) {
        if (*p == '\n') d->lines_drawn++;
    }
}

static void clack_clear(clack_impl_data_t *d) {
    char seq[32];
    if (!d) return;
    if (!d->is_tty || d->lines_drawn <= 0) {
        d->lines_drawn = 0;
        return;
    }
    safe_snprintf(seq, sizeof(seq), "\033[%dA\033[J", d->lines_drawn);
    uart_puts(seq);
    d->lines_drawn = 0;
}

/* 打字机：逐字符输出（非 tty / 动画关闭时整体输出） */
static void clack_typewriter(clack_impl_data_t *d, const char *s, long per_char_us) {
    const char *p;
    int n = 0;
    if (!s) return;
    if (!d || !d->is_tty || !d->anim || per_char_us <= 0) {
        clack_puts_line(d, s);
        return;
    }
    for (p = s; *p; p++) {
        uart_putc(*p);
        if (*p == '\n' && d->is_tty) d->lines_drawn++;
        if (++n % 12 == 0) clack_feed_heartbeat();
        clack_sleep_us(per_char_us);
    }
}

/* 保存进度 spinner（原地刷新，结束后清行不计入块） */
static void clack_spinner(clack_impl_data_t *d, const char *label, int duration_ms) {
    char line[192];
    int frames;
    int i;
    if (!d || !d->is_tty || !d->anim || duration_ms <= 0) return;
    frames = (int)((long)duration_ms * 1000L / CLACK_SPIN_US);
    if (frames < 6) frames = 6;
    for (i = 0; i < frames; i++) {
        safe_snprintf(line, sizeof(line), "\r\033[K%s %s %s%s",
                      COLOR_CYAN, CLACK_SPIN_FRAMES[i % CLACK_SPIN_COUNT],
                      label ? label : "", COLOR_RESET);
        uart_puts(line);
        clack_sleep_us(CLACK_SPIN_US);
        if (i % 6 == 0) clack_feed_heartbeat();
    }
    uart_puts("\r\033[K");
}

/* 呼吸光标（等待输入时的微动画；UTF-8 帧按字符串索引，避免切坏多字节） */
static const char *const CLACK_BREATH_FRAMES[] = {
    "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█", "▇", "▆", "▅", "▄", "▃", "▂", "▁"
};
#define CLACK_BREATH_COUNT ((int)(sizeof(CLACK_BREATH_FRAMES) / sizeof(CLACK_BREATH_FRAMES[0])))

static void clack_breathing(clack_impl_data_t *d, int frames) {
    int i;
    if (!d || !d->is_tty || !d->anim || frames <= 0) return;
    for (i = 0; i < frames; i++) {
        uart_puts("\r\033[K");
        uart_puts(COLOR_DIM);
        uart_puts(CLACK_BAR);
        uart_puts("  ");
        uart_puts(CLACK_BREATH_FRAMES[i % CLACK_BREATH_COUNT]);
        uart_puts(COLOR_RESET);
        clack_sleep_us(CLACK_BREATH_US);
    }
    uart_puts("\r\033[K");
}

/* ============================================================
 * 终端原始模式（逐键；退出/清理必还原）
 * ============================================================ */
static void clack_restore_term(void) {
    if (g_clack_term_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_clack_saved_term);
        g_clack_term_saved = 0;
    }
}

static void clack_raw_begin(clack_impl_data_t *d) {
    struct termios raw;
    if (!d || !d->is_tty || d->raw_active) return;
    if (tcgetattr(STDIN_FILENO, &g_clack_saved_term) != 0) return;
    g_clack_term_saved = 1;
    if (!g_clack_atexit_done) {
        atexit(clack_restore_term);
        g_clack_atexit_done = 1;
    }
    raw = g_clack_saved_term;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) d->raw_active = 1;
}

static void clack_raw_end(clack_impl_data_t *d) {
    if (!d || !d->raw_active) return;
    clack_restore_term();
    d->raw_active = 0;
}

static int clack_fd_wait(int fd, long timeout_us) {
    fd_set fds;
    struct timeval tv;
    int r;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = (time_t)(timeout_us / 1000000L);
    tv.tv_usec = (suseconds_t)(timeout_us % 1000000L);
    r = select(fd + 1, &fds, NULL, NULL, &tv);
    return (r > 0) ? 1 : 0;
}

static int clack_read_key(char *out) {
    unsigned char c = 0;
    if (out) *out = '\0';
    if (read(STDIN_FILENO, &c, 1) != 1) return CLACK_KEY_EOF;

    if (c == 0x1B) {                       /* ESC 或方向键序列 */
        unsigned char s1 = 0;
        unsigned char s2 = 0;
        if (clack_fd_wait(STDIN_FILENO, 40000) &&
            read(STDIN_FILENO, &s1, 1) == 1) {
            if ((s1 == '[' || s1 == 'O') &&
                clack_fd_wait(STDIN_FILENO, 40000) &&
                read(STDIN_FILENO, &s2, 1) == 1) {
                if (s2 == 'A') return CLACK_KEY_UP;
                if (s2 == 'B') return CLACK_KEY_DOWN;
                if (s2 == 'C') return CLACK_KEY_RIGHT;
                if (s2 == 'D') return CLACK_KEY_LEFT;
            }
        }
        if (out) *out = 0x1B;              /* 孤立 ESC = 返回上一步 */
        return CLACK_KEY_CHAR;
    }
    if (out) *out = (char)c;
    return CLACK_KEY_CHAR;
}

/* ============================================================
 * 渲染：标题、描述、徽标、选项
 * ============================================================ */
static void clack_store_step(clack_impl_data_t *d, wizard_step_def_t *step) {
    safe_strncpy(d->cur_step_id, step->id, sizeof(d->cur_step_id));
    safe_strncpy(d->cur_title_en, step->title_en, sizeof(d->cur_title_en));
    safe_strncpy(d->cur_title_zh, step->title_zh, sizeof(d->cur_title_zh));
    safe_strncpy(d->cur_desc_en, step->desc_en, sizeof(d->cur_desc_en));
    safe_strncpy(d->cur_desc_zh, step->desc_zh, sizeof(d->cur_desc_zh));
    safe_strncpy(d->cur_default, step->default_value, sizeof(d->cur_default));
    d->cur_advanced = step->advanced;
    d->cur_skippable = step->skippable;
    d->cur_secret = step->secret;
    d->cur_restart = step->restart_required;
}

static void clack_mark_confirmed(clack_impl_data_t *d) {
    int idx;
    if (!d || d->cur_step_id[0] == '\0') return;
    if (d->confirmed_count >= CLACK_MAX_CONFIRMED) return;
    idx = d->confirmed_count++;
    safe_strncpy(d->confirmed[idx].title_en, d->cur_title_en, sizeof(d->confirmed[idx].title_en));
    safe_strncpy(d->confirmed[idx].title_zh, d->cur_title_zh, sizeof(d->confirmed[idx].title_zh));
}

/* 已确认块（## 绿色）+ 当前块头（# 分隔 + ◆ 标题 + 描述 + 徽标） */
static void clack_draw_head(clack_impl_data_t *d) {
    char line[384];
    const char *primary;
    const char *secondary;
    int i;

    for (i = 0; i < d->confirmed_count; i++) {
        safe_snprintf(line, sizeof(line), "%s##%s %s%s%s %s%s\n",
                      COLOR_GREEN, COLOR_RESET, COLOR_GREEN, CLACK_OK, COLOR_RESET,
                      tr(d->confirmed[i].title_en, d->confirmed[i].title_zh), COLOR_RESET);
        clack_puts_line(d, line);
    }
    if (d->confirmed_count > 0) clack_puts_line(d, "\n");

    safe_snprintf(line, sizeof(line), "%s#%s\n", COLOR_DIM, COLOR_RESET);
    clack_puts_line(d, line);

    primary = tr(d->cur_title_en, d->cur_title_zh);
    secondary = tr(d->cur_title_zh, d->cur_title_en);
    if (secondary && secondary[0] != '\0' && strcmp(primary, secondary) != 0) {
        safe_snprintf(line, sizeof(line), "%s%s%s %s%s %s(%s)%s\n",
                      COLOR_CYAN, COLOR_BOLD, CLACK_ACTIVE, primary, COLOR_RESET,
                      COLOR_DIM, secondary, COLOR_RESET);
    } else {
        safe_snprintf(line, sizeof(line), "%s%s%s %s%s\n",
                      COLOR_CYAN, COLOR_BOLD, CLACK_ACTIVE, primary, COLOR_RESET);
    }
    if (!d->head_animated) {
        clack_typewriter(d, line, CLACK_TYPE_US);
        d->head_animated = 1;
    } else {
        clack_puts_line(d, line);     /* 重绘（方向键/返回）不重播打字机 */
    }

    if (d->cur_desc_en[0] != '\0' || d->cur_desc_zh[0] != '\0') {
        safe_snprintf(line, sizeof(line), "%s  %s%s%s\n",
                      CLACK_BAR, COLOR_DIM, tr(d->cur_desc_en, d->cur_desc_zh), COLOR_RESET);
        clack_puts_line(d, line);
    }
    if (d->cur_advanced) {
        safe_snprintf(line, sizeof(line), "%s  %s%s%s\n",
                      CLACK_BAR, COLOR_DIM, tr("[advanced]", "[高级配置]"), COLOR_RESET);
        clack_puts_line(d, line);
    }
    if (d->cur_restart) {
        safe_snprintf(line, sizeof(line), "%s  %s%s%s\n",
                      CLACK_BAR, COLOR_DIM,
                      tr("[restart required to take effect]", "[修改需重启生效]"), COLOR_RESET);
        clack_puts_line(d, line);
    }
    if (d->cur_skippable) {
        safe_snprintf(line, sizeof(line), "%s  %s%s%s\n",
                      CLACK_BAR, COLOR_DIM,
                      tr("[Enter to skip]", "[可直接回车跳过]"), COLOR_RESET);
        clack_puts_line(d, line);
    }
}

static void clack_draw_options(clack_impl_data_t *d) {
    char line[384];
    int i;
    for (i = 0; i < d->opt_count; i++) {
        const char *label = tr(d->opts[i].label_en, d->opts[i].label_zh);
        if (d->opts[i].is_disabled) {
            safe_snprintf(line, sizeof(line), "%s  %s%s %s %s%s\n",
                          CLACK_BAR, COLOR_DIM, CLACK_RING, label,
                          tr("(unavailable)", "（不可用）"), COLOR_RESET);
        } else if (i == d->opt_sel) {
            safe_snprintf(line, sizeof(line), "%s  %s%s%s %s%s\n",
                          CLACK_BAR, COLOR_CYAN, COLOR_BOLD, CLACK_DOT, label, COLOR_RESET);
        } else {
            safe_snprintf(line, sizeof(line), "%s  %s %s\n",
                          CLACK_BAR, CLACK_RING, label);
        }
        clack_puts_line(d, line);
    }
}

static void clack_redraw(clack_impl_data_t *d) {
    char line[384];
    if (!d) return;
    clack_clear(d);
    clack_draw_head(d);
    if (d->opt_active && d->opt_count > 0) {
        clack_draw_options(d);
        clack_puts_line(d, CLACK_END "\n");
        safe_snprintf(line, sizeof(line), "%s%s  %s%s%s\n", COLOR_DIM, CLACK_BAR,
                      tr("Up/Down move · Enter confirm · Esc back",
                         "↑/↓ 移动 · Enter 确认 · Esc 返回"), COLOR_RESET, "");
        clack_puts_line(d, line);
    } else if (d->cur_secret) {
        safe_snprintf(line, sizeof(line), "%s%s  %s%s\n", COLOR_DIM, CLACK_BAR,
                      tr("Input masked · stored encrypted", "输入掩码 · 落盘加密"), COLOR_RESET);
        clack_puts_line(d, line);
    }
}

/* ============================================================
 * ctx 回调：渲染
 * ============================================================ */
static int clack_render_header(renderer_ctx_t *ctx, wizard_step_def_t *step,
                               int current, int total) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    (void)current;   /* 【先生】不显示步骤编号 */
    (void)total;     /* 【先生】不显示"还剩几步" */
    if (!d || !step) return -1;

    if (d->cur_step_id[0] != '\0' && strcmp(d->cur_step_id, step->id) != 0) {
        clack_mark_confirmed(d);
    }
    clack_store_step(d, step);
    d->opt_active = 0;
    d->opt_count = 0;
    d->navigated = 0;
    d->has_default = 0;
    d->head_animated = 0;        /* 新步骤：重播一次打字机 */
    clack_redraw(d);
    return 0;
}

static int clack_render_options(renderer_ctx_t *ctx, wizard_option_t *options,
                                int count, int selected) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    int i;
    if (!d || !options || count <= 0) return -1;
    if (count > CLACK_MAX_OPTIONS) count = CLACK_MAX_OPTIONS;

    memcpy(d->opts, options, sizeof(wizard_option_t) * (size_t)count);
    d->opt_count = count;
    d->opt_active = 1;
    d->opt_sel = 0;
    d->navigated = 0;
    if (selected >= 0 && selected < count) d->opt_sel = selected;

    /* 默认值高亮（与引擎"回车采用默认"语义一致） */
    d->has_default = 0;
    if (d->cur_default[0] != '\0') {
        for (i = 0; i < count; i++) {
            if (strcmp(d->opts[i].id, d->cur_default) == 0) {
                d->has_default = 1;
                if (!d->opts[i].is_disabled) d->opt_sel = i;
                break;
            }
        }
    }
    if (d->opts[d->opt_sel].is_disabled) {
        for (i = 1; i <= count; i++) {
            int idx = (d->opt_sel + i) % count;
            if (!d->opts[idx].is_disabled) { d->opt_sel = idx; break; }
        }
    }
    clack_redraw(d);
    return 0;
}

static int clack_render_input(renderer_ctx_t *ctx, const char *prompt,
                              char *buf, size_t size) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    char line[384];
    (void)buf;
    (void)size;
    if (!d) return -1;
    if (!prompt || prompt[0] == '\0') prompt = tr("Enter value:", "输入值：");
    safe_snprintf(line, sizeof(line), "%s  %s%s%s\n", CLACK_BAR, COLOR_WHITE, prompt, COLOR_RESET);
    clack_typewriter(d, line, CLACK_TYPE_US);
    clack_puts_line(d, CLACK_BAR "  " COLOR_DIM "> " COLOR_RESET);
    return 0;
}

static int clack_render_message(renderer_ctx_t *ctx, const char *msg, int is_error) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    char line[512];
    if (!d) return -1;
    if (is_error) {
        safe_snprintf(line, sizeof(line), "%s%s %s%s\n", COLOR_RED, CLACK_ERR,
                      msg ? msg : "", COLOR_RESET);
    } else {
        safe_snprintf(line, sizeof(line), "%s%s %s%s\n", COLOR_GREEN, CLACK_OK,
                      msg ? msg : "", COLOR_RESET);
    }
    clack_puts_line(d, line);
    return 0;
}

static int clack_render_complete(renderer_ctx_t *ctx, int success) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    char line[256];
    if (!d) return -1;
    clack_spinner(d, tr("Saving configuration", "正在保存配置"), 720);
    if (success) {
        safe_snprintf(line, sizeof(line), "%s%s  %s %s%s\n",
                      COLOR_GREEN, CLACK_END, CLACK_OK,
                      tr("Configuration saved", "配置已保存"), COLOR_RESET);
    } else {
        safe_snprintf(line, sizeof(line), "%s%s  %s %s%s\n",
                      COLOR_RED, CLACK_END, CLACK_ERR,
                      tr("Configuration failed", "配置失败"), COLOR_RESET);
    }
    clack_puts_line(d, line);
    clack_raw_end(d);
    return 0;
}

/* ============================================================
 * 输入：选择（方向键）/ 文本（打字机 + 掩码）
 * ============================================================ */
static void clack_opt_move(clack_impl_data_t *d, int dir) {
    int i;
    if (!d || d->opt_count <= 0) return;
    for (i = 1; i <= d->opt_count; i++) {
        int idx = (d->opt_sel + dir * i) % d->opt_count;
        if (idx < 0) idx += d->opt_count;
        if (!d->opts[idx].is_disabled) {
            d->opt_sel = idx;
            d->navigated = 1;
            return;
        }
    }
}

static int clack_select(clack_impl_data_t *d, char *buf, size_t size) {
    int result = 0;
    clack_raw_begin(d);
    clack_breathing(d, 6);

    while (result == 0) {
        char ch = 0;
        int key = clack_read_key(&ch);
        if (key == CLACK_KEY_EOF) { result = -1; break; }
        if (key == CLACK_KEY_UP) { clack_opt_move(d, -1); clack_redraw(d); continue; }
        if (key == CLACK_KEY_DOWN) { clack_opt_move(d, 1); clack_redraw(d); continue; }
        if (key == CLACK_KEY_LEFT) {
            safe_strncpy(buf, "ESC", size);
            result = 0;
            break;
        }
        if (key != CLACK_KEY_CHAR) continue;

        if (ch == '\r' || ch == '\n') {
            if (d->opts[d->opt_sel].is_disabled) continue;   /* 禁用项不可确认 */
            /* 高亮项已按默认值初始化 → 直接回传选项号，等价"回车采用默认" */
            safe_snprintf(buf, size, "%d", d->opt_sel + 1);
            break;                                   /* 块尾 └ 已由重绘输出 */
        }
        if (ch == 0x1B) {                            /* Esc = 返回上一步 */
            safe_strncpy(buf, "ESC", size);
            break;
        }
        if (ch == 'q' || ch == 'Q') { safe_strncpy(buf, "q", size); break; }
        if (ch == 0x01) { safe_strncpy(buf, "A", size); result = 1; break; }   /* ^A */
        if (ch == 0x0C) { result = 2; break; }                                  /* ^L */
        if (ch == 0x12) { result = 3; break; }                                  /* ^R */
        if (ch == 0x11) { result = 4; break; }                                  /* ^Q */
        if (ch >= '1' && ch <= '9') {                 /* 数字 = 移动高亮（Enter 确认） */
            int idx = ch - '1';
            if (idx < d->opt_count && !d->opts[idx].is_disabled) {
                d->opt_sel = idx;
                d->navigated = 1;
                clack_redraw(d);
            }
        }
    }
    clack_raw_end(d);
    return result;
}

/* 逐键行输入（原始模式；非 tty 走 cooked 路径） */
static int clack_read_line_raw(clack_impl_data_t *d, char *buf, size_t size) {
    int pos = 0;
    int result = 0;
    clack_raw_begin(d);
    clack_breathing(d, 4);
    /* 呼吸动画会清整行——补回输入提示符（render_input 已画，被清掉） */
    clack_puts_line(d, CLACK_BAR "  " COLOR_DIM "> " COLOR_RESET);

    while (result == 0) {
        char ch = 0;
        int key = clack_read_key(&ch);
        if (key == CLACK_KEY_EOF) { result = -1; break; }
        if (key != CLACK_KEY_CHAR) continue;         /* 输入行忽略方向键 */

        if (ch == '\r' || ch == '\n') {
            if (pos == 0) {
                d->empty_input_count++;
                if (d->empty_input_count >= 2 && d->empty_input_count < 5) {
                    clack_puts_line(d, COLOR_YELLOW
                        "\n  ⚠ No input detected. Enter twice more to switch mode.\n"
                        "  ⚠ 未检测到输入。再按两次回车将切换模式。\n" COLOR_RESET);
                }
                if (d->empty_input_count >= 5) { result = -3; break; }
                buf[0] = '\0';
                clack_puts_line(d, "\n");
                break;   /* 空输入交回引擎（采用默认 / 提示尚未选择），与 CLI 同语义 */
            }
            d->empty_input_count = 0;
            buf[pos] = '\0';
            clack_puts_line(d, "\n" CLACK_END "\n");
            break;
        }
        if (ch == 127 || ch == 8) {
            if (pos > 0) {
                safe_backspace_echo(buf, &pos);
                if (d->cur_secret) uart_puts(COLOR_RESET);
            }
            continue;
        }
        if (ch == 0x1B) { safe_strncpy(buf, "ESC", size); clack_puts_line(d, "\n"); break; }
        if (ch == 'q' || ch == 'Q') { safe_strncpy(buf, "q", size); result = 0; break; }
        if (ch == 0x01) { safe_strncpy(buf, "A", size); result = 1; break; }
        if (ch == 0x0C) { result = 2; break; }
        if (ch == 0x12) { result = 3; break; }
        if (ch == 0x11) { result = 4; break; }
        if ((unsigned char)ch >= 32 && pos < (int)size - 1) {
            buf[pos++] = ch;
            uart_putc(d->cur_secret ? '*' : ch);     /* 私密项掩码回显 */
        }
    }
    clack_raw_end(d);
    return result;
}

/* 非 tty / 管道：按行读取（cooked 模式，语义与 CLI 渲染器一致） */
static int clack_read_line_cooked(clack_impl_data_t *d, char *buf, size_t size) {
    int pos = 0;
    int result = 0;
    while (result == 0) {
        char c = uart_getc();
        if (c == 0) { result = -1; break; }
        if (c == 0x01) { safe_strncpy(buf, "A", size); result = 1; break; }
        if (c == 0x0C) { result = 2; break; }
        if (c == 0x12) { result = 3; break; }
        if (c == 0x11) { result = 4; break; }
        if (c == '\r' || c == '\n') {
            if (pos == 0) {
                d->empty_input_count++;
                if (d->empty_input_count >= 5) { result = -3; break; }
                buf[0] = '\0';
                break;   /* 与 CLI 渲染器同语义 */
            }
            d->empty_input_count = 0;
            buf[pos] = '\0';
            clack_puts_line(d, "\n" CLACK_END "\n");
            break;
        }
        if (c == 127 || c == 8) {
            if (pos > 0) safe_backspace_echo(buf, &pos);
            continue;
        }
        if (c >= 32 && c <= 126 && pos < (int)size - 1) {
            buf[pos++] = c;
            uart_putc(c);
        }
    }
    return result;
}

static int clack_get_input(renderer_ctx_t *ctx, char *buf, size_t size) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    if (!d || !buf || size == 0) return -1;
    if (d->opt_active && d->opt_count > 0) return clack_select(d, buf, size);
    buf[0] = '\0';
    if (!d->is_tty) return clack_read_line_cooked(d, buf, size);
    return clack_read_line_raw(d, buf, size);
}

static int clack_wait_key(renderer_ctx_t *ctx) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    char ch = 0;
    clack_puts_line(d, tr("Press any key to continue...", "按任意键继续..."));
    clack_breathing(d, 8);
    clack_raw_begin(d);
    (void)clack_read_key(&ch);
    clack_raw_end(d);
    clack_puts_line(d, "\n");
    return 0;
}

/* ============================================================
 * 生命周期 / 自检
 * ============================================================ */
static void clack_cleanup(renderer_ctx_t *ctx) {
    clack_impl_data_t *d;
    if (!ctx) return;
    d = (clack_impl_data_t *)ctx->impl;
    if (d) {
        clack_raw_end(d);
        free(d);
        ctx->impl = NULL;
    }
}

static int clack_self_test(renderer_ctx_t *ctx) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    LOG_INFO_T("Clack", "SelfTest", "Start", "Clack renderer self-test");
    if (!d || !d->initialized) {
        LOG_ERROR_T("Clack", "SelfTest", "NoImpl", "impl data missing");
        return -1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        LOG_WARN_T("Clack", "SelfTest", "NoTTY", "not a terminal (plain fallback active)");
        return -1;
    }
    LOG_INFO_T("Clack", "SelfTest", "OK", "Clack renderer is healthy");
    return 0;
}

static int clack_is_healthy(renderer_ctx_t *ctx) {
    clack_impl_data_t *d = (clack_impl_data_t *)ctx->impl;
    /* 非 tty 时自带纯文本回退，不阻塞向导输入 */
    return (d && d->initialized) ? 1 : 0;
}

/* ============================================================
 * 创建
 * ============================================================ */
int renderer_clack_impl_create(renderer_ctx_t *ctx) {
    clack_impl_data_t *d;
    if (!ctx) return -1;
    d = (clack_impl_data_t *)calloc(1, sizeof(clack_impl_data_t));
    if (!d) return -1;

    d->initialized = 1;
    d->is_tty = (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) ? 1 : 0;
    d->anim = d->is_tty;
    d->opt_sel = 0;

    ctx->impl = d;
    ctx->render_header = clack_render_header;
    ctx->render_options = clack_render_options;
    ctx->render_input = clack_render_input;
    ctx->render_message = clack_render_message;
    ctx->render_complete = clack_render_complete;
    ctx->get_input = clack_get_input;
    ctx->wait_key = clack_wait_key;
    ctx->cleanup = clack_cleanup;
    ctx->self_test = clack_self_test;
    ctx->is_healthy = clack_is_healthy;

    LOG_INFO_T("Clack", "Create", "OK", "Clack renderer created (tty=%d, anim=%d)",
               d->is_tty, d->anim);
    return 0;
}

int renderer_clack_available(void) {
    return (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) ? 1 : 0;
}
