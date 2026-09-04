/**
 * @file    src/tui/tui_renderer.c
 * @brief   Notcurses 渲染器实现（适配 3.x API + 键盘防抖动 + 防御检查 + 健康计数）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 所有渲染函数返回状态码；增加防御检查；适配 ncplane_create；
 *          增加渲染超时检测；双文支持；
 *          使用 NCOPTION_SUPPRESS_BANNERS 抑制横幅；
 *          增加 render_count, render_fail_count, last_render_time 健康监控。
 */

#include "tui_renderer.h"
#include "tui_defensive.h"
#include "tui_resource.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <notcurses/notcurses.h>

#define TUI_COLOR_ACCENT    0x00ffff
#define TUI_COLOR_GRAY      0x888888
#define TUI_COLOR_WHITE     0xffffff
#define TUI_COLOR_BG_DARK   0x0d0d2a

static int last_key_time = 0;
static int last_key = 0;
static const int KEY_DEBOUNCE_MS = 50;

/* ============================================================
 * FTF[获取当前时间戳（毫秒）]
 * ============================================================ */
static int get_tick_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        LOG_ERROR_T("TUIRenderer", "GetTick", "Fail", "clock_gettime failed: %s", strerror(errno));
        return 0;
    }
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ============================================================
 * FTF[创建 ncplane（带错误日志）]
 * ============================================================ */
static struct ncplane* create_ncplane(struct ncplane *parent, int rows, int cols,
                                      int y, int x, const char *name) {
    LOG_DEBUG_T("TUIRenderer", "CreatePlane", "Enter", "name='%s'", name ? name : "(null)");
    if (!parent) {
        LOG_ERROR_T("TUIRenderer", "CreatePlane", "NullParent", "parent is NULL");
        return NULL;
    }
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (y < 0) y = 0;
    if (x < 0) x = 0;

    struct ncplane_options nopts = {
        .y = y,
        .x = x,
        .rows = rows,
        .cols = cols,
        .userptr = NULL,
        .name = name,
        .resizecb = NULL,
        .flags = 0,
    };

    struct ncplane *plane = ncplane_create(parent, &nopts);
    if (!plane) {
        LOG_ERROR_T("TUIRenderer", "CreatePlane", "Fail", "ncplane_create failed for '%s'", name ? name : "(null)");
        return NULL;
    }
    return plane;
}

/* ============================================================
 * FTF[初始化 Notcurses 渲染器]
 * ============================================================ */
int tui_renderer_init(tui_renderer_t *renderer) {
    LOG_INFO_T("TUIRenderer", "Init", "Enter", "renderer=%p", (void*)renderer);
    if (!renderer) {
        LOG_ERROR_T("TUIRenderer", "Init", "NullRenderer", "renderer is NULL");
        return -1;
    }

    memset(renderer, 0, sizeof(tui_renderer_t));

    struct notcurses_options opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS,
        .loglevel = NCLOGLEVEL_FATAL,
    };

    renderer->nc = notcurses_init(&opts, NULL);
    if (!renderer->nc) {
        LOG_ERROR_T("TUIRenderer", "Init", "Fail", "notcurses_init returned NULL: %s", strerror(errno));
        return -1;
    }

    renderer->stdplane = notcurses_stdplane(renderer->nc);
    if (!renderer->stdplane) {
        notcurses_stop(renderer->nc);
        LOG_ERROR_T("TUIRenderer", "Init", "NoStdPlane", "notcurses_stdplane returned NULL");
        return -1;
    }

    unsigned int width_u, height_u;
    ncplane_dim_yx(renderer->stdplane, &height_u, &width_u);
    renderer->height = height_u;
    renderer->width = width_u;

    if (renderer->height < 5) renderer->height = 24;
    if (renderer->width < 20) renderer->width = 80;

    renderer->title_height = 3;
    renderer->status_height = 1;
    renderer->help_height = 1;

    int available_rows = (int)(renderer->height - renderer->title_height -
                               renderer->status_height - renderer->help_height - 2);
    int available_cols = (int)(renderer->width - 2);
    renderer->main_rows = (available_rows > 1) ? available_rows : 1;
    renderer->main_cols = (available_cols > 1) ? available_cols : 1;

    renderer->main_plane = create_ncplane(renderer->stdplane, renderer->main_rows,
                                          renderer->main_cols, renderer->title_height + 1,
                                          1, "main_plane");
    if (!renderer->main_plane) {
        notcurses_stop(renderer->nc);
        return -1;
    }
    tui_resource_register(renderer->main_plane);

    renderer->status_plane = create_ncplane(renderer->stdplane, renderer->status_height,
                                            (int)renderer->width,
                                            (int)(renderer->height - renderer->status_height -
                                                  renderer->help_height - 1),
                                            0, "status_plane");
    if (!renderer->status_plane) {
        tui_resource_cleanup_all();
        notcurses_stop(renderer->nc);
        return -1;
    }
    tui_resource_register(renderer->status_plane);

    renderer->help_plane = create_ncplane(renderer->stdplane, renderer->help_height,
                                          (int)renderer->width,
                                          (int)(renderer->height - renderer->help_height - 1),
                                          0, "help_plane");
    if (!renderer->help_plane) {
        tui_resource_cleanup_all();
        notcurses_stop(renderer->nc);
        return -1;
    }
    tui_resource_register(renderer->help_plane);

    renderer->title_plane = create_ncplane(renderer->stdplane, renderer->title_height,
                                           (int)renderer->width, 0, 0, "title_plane");
    if (!renderer->title_plane) {
        tui_resource_cleanup_all();
        notcurses_stop(renderer->nc);
        return -1;
    }
    tui_resource_register(renderer->title_plane);

    ncplane_set_bg_rgb(renderer->title_plane, 0x1a1a2e);
    ncplane_set_fg_rgb(renderer->title_plane, TUI_COLOR_ACCENT);
    ncplane_putstr_aligned(renderer->title_plane, 0, NCALIGN_CENTER,
                           tr("LING OS Setup Wizard", "LING OS 设置向导"));
    ncplane_set_fg_rgb(renderer->title_plane, TUI_COLOR_GRAY);
    ncplane_putstr_aligned(renderer->title_plane, 1, NCALIGN_CENTER,
                           tr("Version: LN-0.4.3", "版本：LN-0.4.3"));
    ncplane_set_fg_rgb(renderer->title_plane, 0x444444);
    /* 修正：分隔线绘制前将光标归位到第 3 行起点，避免与版本号粘连 */
    ncplane_cursor_move_yx(renderer->title_plane, 2, 0);
    for (int i = 0; i < (int)renderer->width; i++) {
        ncplane_putstr(renderer->title_plane, "─");
    }

    ncplane_set_bg_rgb(renderer->status_plane, 0x1a1a2e);
    ncplane_set_fg_rgb(renderer->status_plane, TUI_COLOR_GRAY);
    /* 修正：状态栏仅 1 行，文本绘制在 y=0（此前 y=1 越界导致不显示） */
    ncplane_putstr_aligned(renderer->status_plane, 0, NCALIGN_LEFT,
                           tr("[Press ESC to cancel, ↑/↓ to navigate]",
                              "[按 ESC 取消，↑/↓ 导航]"));

    ncplane_set_bg_rgb(renderer->help_plane, 0x0d0d1a);
    ncplane_set_fg_rgb(renderer->help_plane, 0x666666);
    /* 修正：帮助栏仅 1 行，文本绘制在 y=0（此前 y=1 越界导致不显示） */
    ncplane_putstr_aligned(renderer->help_plane, 0, NCALIGN_LEFT,
                           tr("Enter: confirm  ESC: cancel  Q: quit",
                              "Enter: 确认  ESC: 取消  Q: 退出"));

    /* ---- 健康计数初始化 ---- */
    renderer->render_count = 0;
    renderer->render_fail_count = 0;
    renderer->last_render_time = time(NULL);

    tui_renderer_refresh(renderer);
    LOG_INFO_T("TUIRenderer", "Init", "OK", "width=%u height=%u", renderer->width, renderer->height);
    return 0;
}

/* ============================================================
 * FTF[销毁 Notcurses 渲染器]
 * ============================================================ */
void tui_renderer_destroy(tui_renderer_t *renderer) {
    if (!renderer) return;
    if (renderer->main_plane) {
        ncplane_destroy(renderer->main_plane);
        renderer->main_plane = NULL;
    }
    if (renderer->status_plane) {
        ncplane_destroy(renderer->status_plane);
        renderer->status_plane = NULL;
    }
    if (renderer->help_plane) {
        ncplane_destroy(renderer->help_plane);
        renderer->help_plane = NULL;
    }
    if (renderer->title_plane) {
        ncplane_destroy(renderer->title_plane);
        renderer->title_plane = NULL;
    }
    if (renderer->nc) {
        notcurses_stop(renderer->nc);
        renderer->nc = NULL;
    }
    memset(renderer, 0, sizeof(tui_renderer_t));
}

/* ============================================================
 * FTF[刷新渲染（含健康计数更新）]
 * ============================================================ */
void tui_renderer_refresh(tui_renderer_t *renderer) {
    if (!renderer || !renderer->nc) return;
    renderer->render_count++;
    int ret = notcurses_render(renderer->nc);
    if (ret == 0) {
        renderer->last_render_time = time(NULL);
        renderer->render_fail_count = 0;
    } else {
        renderer->render_fail_count++;
        LOG_WARN_T("TUIRenderer", "Refresh", "RenderFail",
                   "notcurses_render returned %d (fail_count=%d)", ret, renderer->render_fail_count);
    }
}

/* ============================================================
 * FTF[清空主平面]
 * ============================================================ */
int tui_renderer_clear_main(tui_renderer_t *renderer) {
    if (!renderer || !renderer->main_plane) return -1;
    ncplane_erase(renderer->main_plane);
    return 0;
}

/* ============================================================
 * FTF[设置状态栏文本]
 * ============================================================ */
int tui_renderer_set_status(tui_renderer_t *renderer, const char *text) {
    if (!renderer || !renderer->status_plane) return -1;
    if (!text) {
        LOG_WARN_T("TUIRenderer", "SetStatus", "NullText", "status text is NULL");
        return -1;
    }
    ncplane_erase(renderer->status_plane);
    ncplane_set_fg_rgb(renderer->status_plane, TUI_COLOR_GRAY);
    ncplane_putstr_aligned(renderer->status_plane, 0, NCALIGN_LEFT, text);
    tui_renderer_refresh(renderer);
    return 0;
}

/* ============================================================
 * FTF[设置帮助栏文本]
 * ============================================================ */
int tui_renderer_set_help(tui_renderer_t *renderer, const char *text) {
    if (!renderer || !renderer->help_plane) return -1;
    if (!text) {
        LOG_WARN_T("TUIRenderer", "SetHelp", "NullText", "help text is NULL");
        return -1;
    }
    ncplane_erase(renderer->help_plane);
    ncplane_set_fg_rgb(renderer->help_plane, 0x666666);
    ncplane_putstr_aligned(renderer->help_plane, 0, NCALIGN_LEFT, text);
    tui_renderer_refresh(renderer);
    return 0;
}

/* ============================================================
 * FTF[获取主平面尺寸]
 * ============================================================ */
void tui_renderer_get_main_size(tui_renderer_t *renderer, int *rows, int *cols) {
    if (renderer) {
        if (rows) *rows = renderer->main_rows;
        if (cols) *cols = renderer->main_cols;
    } else {
        if (rows) *rows = 0;
        if (cols) *cols = 0;
    }
}

/* ============================================================
 * FTF[带超时的渲染]
 * ============================================================ */
int tui_renderer_render_with_timeout(tui_renderer_t *renderer, int timeout_ms) {
    if (!renderer || !renderer->nc) return -1;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int ret = notcurses_render(renderer->nc);
    if (ret != 0) {
        LOG_WARN_T("TUIRenderer", "RenderWithTimeout", "RenderFail",
                   "notcurses_render returned %d", ret);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                   (now.tv_nsec - start.tv_nsec) / 1000000;

    if (elapsed > timeout_ms) {
        LOG_WARN_T("TUIRenderer", "RenderWithTimeout", "Timeout",
                   "render took %ldms (limit %dms)", elapsed, timeout_ms);
        return -1;
    }
    return 0;
}

/* ============================================================
 * FTF[获取键盘输入（防抖动）]
 * ============================================================ */
int tui_renderer_get_key(tui_renderer_t *renderer, int timeout_ms) {
    if (!renderer) return -1;

    struct timespec ts;
    struct timespec *ts_ptr = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    ncinput input;
    memset(&input, 0, sizeof(input));
    int key = notcurses_get(renderer->nc, ts_ptr, &input);

    if (key == 0) return 0;
    if (key == -1) return -1;

    int now = get_tick_ms();
    if (key == last_key && (now - last_key_time) < KEY_DEBOUNCE_MS) {
        return 0;
    }
    last_key = key;
    last_key_time = now;

    /* 【修复】Ctrl+字母 → 控制码（^A=1 ... ^Z=26）；notcurses 返回字母+ctrl 修饰 */
    if (input.ctrl && key >= 'a' && key <= 'z') {
        return key - 'a' + 1;
    }
    if (input.ctrl && key >= 'A' && key <= 'Z') {
        return key - 'A' + 1;
    }

    int mapped_key;
    switch (key) {
        case 27:          mapped_key = 27; break;
        case 10:          mapped_key = 10; break;
        case 13:          mapped_key = 13; break;
        case NCKEY_ENTER: mapped_key = 10; break;   /* 【修复】Enter 统一为 10 */
        case 32:          mapped_key = 32; break;
        case 9:           mapped_key = 9; break;
        case 127:         mapped_key = 127; break;
        case NCKEY_UP:    mapped_key = 1000; break;
        case NCKEY_DOWN:  mapped_key = 1001; break;
        case NCKEY_LEFT:  mapped_key = 1002; break;
        case NCKEY_RIGHT: mapped_key = 1003; break;
        default:
            if (key >= 'a' && key <= 'z') mapped_key = key;
            else if (key >= 'A' && key <= 'Z') mapped_key = key;
            else if (key >= '0' && key <= '9') mapped_key = key;
            else mapped_key = key;
            break;
    }
    return mapped_key;
}

/* ============================================================
 * FTF[判断是否为退出键]
 * ============================================================ */
int tui_renderer_is_exit_key(int key) {
    return (key == 27 || key == 'q' || key == 'Q');
}