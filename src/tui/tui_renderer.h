/**
 * @file    src/tui/tui_renderer.h
 * @brief   Notcurses 渲染器声明
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C3, AI-CTL
 * @changes 添加 render_count、render_fail_count、last_render_time 成员
 */

#ifndef TUI_TUI_RENDERER_H
#define TUI_TUI_RENDERER_H

#include <notcurses/notcurses.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 渲染器结构
 * ============================================================ */
typedef struct tui_renderer {
    struct notcurses *nc;
    struct ncplane *stdplane;
    struct ncplane *main_plane;
    struct ncplane *status_plane;
    struct ncplane *help_plane;
    struct ncplane *title_plane;
    unsigned int width;
    unsigned int height;
    int main_rows;
    int main_cols;
    int status_height;
    int help_height;
    int title_height;

    /* 健康检查相关（新增） */
    int render_count;          /* 渲染次数 */
    int render_fail_count;     /* 连续失败次数 */
    time_t last_render_time;   /* 上次成功渲染时间 */
} tui_renderer_t;

/* ============================================================
 * 渲染器 API
 * ============================================================ */
int tui_renderer_init(tui_renderer_t *renderer);
void tui_renderer_destroy(tui_renderer_t *renderer);
void tui_renderer_refresh(tui_renderer_t *renderer);
int tui_renderer_clear_main(tui_renderer_t *renderer);
int tui_renderer_set_status(tui_renderer_t *renderer, const char *text);
int tui_renderer_set_help(tui_renderer_t *renderer, const char *text);
void tui_renderer_get_main_size(tui_renderer_t *renderer, int *rows, int *cols);
int tui_renderer_render_with_timeout(tui_renderer_t *renderer, int timeout_ms);
int tui_renderer_get_key(tui_renderer_t *renderer, int timeout_ms);
int tui_renderer_is_exit_key(int key);

#ifdef __cplusplus
}
#endif

#endif /* TUI_TUI_RENDERER_H */