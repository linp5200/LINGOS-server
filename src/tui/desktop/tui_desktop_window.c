/**
 * @file    tui_desktop_window.c
 * @brief   TUI 桌面窗口管理器（创建、拖动、聚焦、关闭）
 * @version LN-B-4.2.0.0
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 全局窗口管理
 * ============================================================ */

static tui_window_t *g_windows = NULL;
static int g_window_count = 0;
static int g_window_id_counter = 1;
static tui_window_t *g_focused_window = NULL;

/* ============================================================
 * 内部辅助：查找窗口
 * ============================================================ */

static tui_window_t* find_window_by_id(uint32_t id) {
    tui_window_t *w = g_windows;
    while (w) {
        if (w->id == id) return w;
        w = w->next;
    }
    return NULL;
}

static tui_window_t* find_window_by_plane(struct ncplane *plane) {
    tui_window_t *w = g_windows;
    while (w) {
        if (w->plane == plane) return w;
        w = w->next;
    }
    return NULL;
}

/* ============================================================
 * 内部辅助：提升窗口到最前
 * ============================================================ */

static void raise_window(tui_window_t *win) {
    if (!win) return;

    int max_z = 0;
    tui_window_t *w = g_windows;
    while (w) {
        if (w->z_order > max_z) max_z = w->z_order;
        w = w->next;
    }
    win->z_order = max_z + 1;
    win->focused = 1;

    w = g_windows;
    while (w) {
        if (w != win) w->focused = 0;
        w = w->next;
    }
    g_focused_window = win;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

tui_window_t* tui_window_create(const char *title, int x, int y, int width, int height) {
    LOG_INFO_T("TUIWindow", "Create", "Enter", "title='%s', x=%d, y=%d, w=%d, h=%d",
               title ? title : "(null)", x, y, width, height);

    if (!title || width < 10 || height < 5) {
        LOG_ERROR_T("TUIWindow", "Create", "Invalid", "invalid parameters");
        return NULL;
    }

    tui_desktop_state_t *state = tui_desktop_get_state();
    if (!state || !state->nc) {
        LOG_ERROR_T("TUIWindow", "Create", "NoState", "desktop state not available");
        return NULL;
    }

    tui_window_t *win = calloc(1, sizeof(tui_window_t));
    if (!win) {
        LOG_ERROR_T("TUIWindow", "Create", "MallocFail", "calloc failed");
        return NULL;
    }

    win->id = g_window_id_counter++;
    safe_strncpy(win->title, title, sizeof(win->title));
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    win->z_order = g_window_count + 1;
    win->focused = 0;
    win->draggable = 1;
    win->is_dragging = 0;
    win->on_close = NULL;
    win->on_resize = NULL;
    win->user_data = NULL;
    win->next = NULL;

    struct ncplane *parent = state->desktop_plane;
    if (!parent) {
        LOG_ERROR_T("TUIWindow", "Create", "NoParent", "desktop_plane is NULL");
        free(win);
        return NULL;
    }

    struct ncplane_options nopts = {
        .y = y,
        .x = x,
        .rows = height,
        .cols = width,
        .userptr = NULL,
        .name = NULL,
        .resizecb = NULL,
        .flags = 0,
    };
    win->plane = ncplane_create(parent, &nopts);
    if (!win->plane) {
        LOG_ERROR_T("TUIWindow", "Create", "PlaneFail", "ncplane_create failed");
        free(win);
        return NULL;
    }

    ncplane_set_bg_rgb(win->plane, 0x0d0d2a);
    ncplane_set_fg_rgb(win->plane, 0xcccccc);

    tui_window_draw(win);

    win->next = g_windows;
    g_windows = win;
    g_window_count++;

    raise_window(win);

    LOG_INFO_T("TUIWindow", "Create", "OK", "window %u created", win->id);
    return win;
}

void tui_window_destroy(tui_window_t *win) {
    LOG_INFO_T("TUIWindow", "Destroy", "Enter", "window %u", win ? win->id : 0);

    if (!win) {
        LOG_WARN_T("TUIWindow", "Destroy", "Invalid", "win is NULL");
        return;
    }

    tui_window_t **pp = &g_windows;
    while (*pp) {
        if (*pp == win) {
            *pp = win->next;
            break;
        }
        pp = &(*pp)->next;
    }
    g_window_count--;

    if (win->plane) {
        ncplane_destroy(win->plane);
        win->plane = NULL;
    }

    if (g_focused_window == win) {
        g_focused_window = NULL;
    }

    free(win);
    LOG_DEBUG_T("TUIWindow", "Destroy", "OK", "window destroyed");
}

void tui_window_draw(tui_window_t *win) {
    if (!win || !win->plane) {
        LOG_WARN_T("TUIWindow", "Draw", "Invalid", "win or plane is NULL");
        return;
    }

    struct ncplane *p = win->plane;
    int w = ncplane_dim_x(p);
    int h = ncplane_dim_y(p);

    ncplane_erase(p);

    uint64_t border_color = win->focused ? 0x00ccff : 0x444466;
    uint64_t title_bg = win->focused ? 0x002244 : 0x111133;
    uint64_t title_fg = win->focused ? 0xffffff : 0x888888;

    ncplane_set_fg_rgb(p, border_color);

    ncplane_cursor_move_yx(p, 0, 0);
    ncplane_putstr(p, "┌");
    for (int i = 1; i < w - 1; i++) {
        ncplane_putstr(p, "─");
    }
    ncplane_putstr(p, "┐");

    for (int i = 1; i < h - 1; i++) {
        ncplane_cursor_move_yx(p, i, 0);
        ncplane_putstr(p, "│");
        ncplane_cursor_move_yx(p, i, w - 1);
        ncplane_putstr(p, "│");
    }

    ncplane_cursor_move_yx(p, h - 1, 0);
    ncplane_putstr(p, "└");
    for (int i = 1; i < w - 1; i++) {
        ncplane_putstr(p, "─");
    }
    ncplane_putstr(p, "┘");

    ncplane_set_bg_rgb(p, title_bg);
    ncplane_set_fg_rgb(p, title_fg);

    int title_len = strlen(win->title);
    int title_start = 2;
    if (title_start + title_len > w - 4) {
        title_len = w - 6;
    }

    ncplane_cursor_move_yx(p, 0, title_start);
    ncplane_set_styles(p, NCSTYLE_BOLD);
    ncplane_putstr(p, win->title);
    ncplane_set_styles(p, 0);

    ncplane_set_fg_rgb(p, 0xff6666);
    ncplane_cursor_move_yx(p, 0, w - 3);
    ncplane_putstr(p, "X");
    ncplane_set_fg_rgb(p, border_color);

    ncplane_set_bg_rgb(p, 0x0d0d2a);
}

void tui_window_set_title(tui_window_t *win, const char *title) {
    if (!win || !title) return;
    safe_strncpy(win->title, title, sizeof(win->title));
    tui_window_draw(win);
}

void tui_window_focus(tui_window_t *win) {
    if (!win) return;
    if (g_focused_window == win && win->focused) return;
    raise_window(win);
    tui_window_draw(win);
    tui_desktop_refresh(tui_desktop_get_state());
}

int tui_window_handle_mouse(tui_window_t *win, int event_type, int mouse_x, int mouse_y) {
    if (!win || !win->plane) return 0;

    int x = win->x;
    int y = win->y;
    int w = ncplane_dim_x(win->plane);
    int h = ncplane_dim_y(win->plane);

    if (mouse_x < x || mouse_x >= x + w || mouse_y < y || mouse_y >= y + h) {
        return 0;
    }

    int local_x = mouse_x - x;
    int local_y = mouse_y - y;

    if (local_y == 0 && local_x >= w - 3 && local_x < w - 1) {
        if (event_type == 1) {
            LOG_INFO_T("TUIWindow", "Mouse", "Close", "window %u close clicked", win->id);
            if (win->on_close) {
                win->on_close(win);
            } else {
                tui_window_destroy(win);
            }
            return 1;
        }
    }

    if (local_y == 0 && local_x >= 2 && local_x < w - 3) {
        if (event_type == 1) {
            if (win->draggable) {
                win->is_dragging = 1;
                win->drag_off_x = local_x;
                win->drag_off_y = local_y;
                tui_window_focus(win);
                LOG_DEBUG_T("TUIWindow", "Mouse", "DragStart", "window %u drag start", win->id);
            }
            return 1;
        } else if (event_type == 0) {
            win->is_dragging = 0;
            LOG_DEBUG_T("TUIWindow", "Mouse", "DragEnd", "window %u drag end", win->id);
            return 1;
        }
    }

    if (event_type == 1) {
        tui_window_focus(win);
        return 1;
    }

    return 0;
}

void tui_window_update_drag(tui_window_t *win, int mouse_x, int mouse_y) {
    if (!win || !win->is_dragging) return;

    int new_x = mouse_x - win->drag_off_x;
    int new_y = mouse_y - win->drag_off_y;

    tui_desktop_state_t *state = tui_desktop_get_state();
    if (state) {
        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        int max_x = state->width - win->width;
        int max_y = state->height - win->height;
        if (new_x > max_x) new_x = max_x;
        if (new_y > max_y) new_y = max_y;
    }

    if (new_x != win->x || new_y != win->y) {
        win->x = new_x;
        win->y = new_y;
        ncplane_move_yx(win->plane, new_y, new_x);
    }
}

tui_window_t* tui_window_get_focused(void) {
    return g_focused_window;
}

tui_window_t* tui_window_get_first(void) {
    return g_windows;
}

int tui_window_count(void) {
    return g_window_count;
}

void tui_window_set_on_close(tui_window_t *win, void (*on_close)(tui_window_t *)) {
    if (win) win->on_close = on_close;
}

void tui_window_set_user_data(tui_window_t *win, void *data) {
    if (win) win->user_data = data;
}

void* tui_window_get_user_data(tui_window_t *win) {
    return win ? win->user_data : NULL;
}