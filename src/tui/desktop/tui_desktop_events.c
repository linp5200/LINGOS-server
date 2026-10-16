/**
 * @file    src/tui/desktop/tui_desktop_events.c
 * @brief   TUI 桌面事件处理（鼠标 + 键盘）
 * @version LN-B-5.0.0.0
 * @changes 坐标映射修复；双文支持；鼠标事件处理完善
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "tui_desktop_icons.h"
#include "tui_desktop_taskbar.h"
#include "log_extra.h"
#include "safe_string.h"
#include "lang.h"
#include "uart.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef NCEV_BUTTON_PRESS
#define NCEV_BUTTON_PRESS   1
#define NCEV_BUTTON_RELEASE 2
#define NCEV_MOTION         3
#endif

typedef struct {
    int mouse_x;
    int mouse_y;
    int mouse_btn;
    int mouse_clicked;
    int drag_active;
    tui_window_t *drag_window;
} event_state_t;

static event_state_t g_events;
/* 【第9期】最近一次悬停的图标——用于"仅在悬停目标变化时刷新"，避免鼠标移动即全屏重绘 */
static tui_icon_t *g_last_hover_icon = NULL;

static int is_in_desktop_area(tui_desktop_state_t *state, int x, int y) {
    (void)x;   /* 【第9期】仅按 y 判定；显式标注避免 -Wextra 噪音 */
    if (!state) return 0;
    int taskbar_y = state->height - (state->config.show_taskbar ? 1 : 0);
    return (y < taskbar_y);
}

static void process_mouse_event(tui_desktop_state_t *state, int key) {
    (void)key;   /* 【第9期】鼠标路径不消费按键码；显式标注避免 -Wextra 噪音 */
    int x = g_events.mouse_x;
    int y = g_events.mouse_y;

    if (x < 0 || y < 0) return;

    if (g_events.drag_active && g_events.drag_window) {
        tui_window_update_drag(g_events.drag_window, x, y);
        tui_desktop_refresh(state);
        return;
    }

    if (!is_in_desktop_area(state, x, y)) {
        if (state->config.show_taskbar) {
            if (tui_desktop_taskbar_handle_mouse(1, x, y)) {
                tui_desktop_refresh(state);
                return;
            }
        }
        return;
    }

    /* 窗口点击检测（从最上层到最下层） */
    tui_window_t *win = tui_window_get_first();
    tui_window_t *hit_win = NULL;
    int max_z = -1;

    while (win) {
        int wx = win->x;
        int wy = win->y;
        int ww = ncplane_dim_x(win->plane);
        int wh = ncplane_dim_y(win->plane);

        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
            if (win->z_order > max_z) {
                max_z = win->z_order;
                hit_win = win;
            }
        }
        win = tui_window_get_next(win);
    }

    if (hit_win) {
        if (tui_window_handle_mouse(hit_win, 1, x, y)) {
            tui_desktop_refresh(state);
            return;
        }
        tui_window_focus(hit_win);
        tui_desktop_refresh(state);
        return;
    }

    if (tui_desktop_icons_handle_mouse(1, x, y)) {
        tui_desktop_refresh(state);
        return;
    }

    tui_window_focus(NULL);
    tui_desktop_refresh(state);
}

void tui_desktop_events_init(void) {
    LOG_INFO_T("TUIDesktopEvents", "Init", "Enter", "initializing event system");
    memset(&g_events, 0, sizeof(g_events));
    g_events.mouse_x = -1;
    g_events.mouse_y = -1;
    g_events.drag_active = 0;
    g_events.drag_window = NULL;
    LOG_INFO_T("TUIDesktopEvents", "Init", "OK", "event system ready");
}

int tui_desktop_events_handle_key(tui_desktop_state_t *state, int key) {
    if (!state) return -1;

    switch (key) {
        case 'q':
        case 'Q':
            uart_puts(tr("Exit desktop? (y/N): ", "退出桌面？(y/N): "));
            char c = uart_getc();
            uart_putc(c);
            uart_puts("\n");
            if (c == 'y' || c == 'Y') {
                tui_desktop_set_exit(state, 0);
            }
            return 1;

        case 27:
            if (g_events.drag_active) {
                g_events.drag_active = 0;
                g_events.drag_window = NULL;
            }
            return 1;

        case NCKEY_UP:
        case NCKEY_DOWN:
        case NCKEY_LEFT:
        case NCKEY_RIGHT:
            /* 用于菜单导航等，可扩展 */
            return 0;

        default:
            return 0;
    }
}

int tui_desktop_events_handle_mouse(tui_desktop_state_t *state, int key, ncinput *input) {
    if (!state || !input) return -1;

    int x = input->x;
    int y = input->y;

    if (x != g_events.mouse_x || y != g_events.mouse_y) {
        g_events.mouse_x = x;
        g_events.mouse_y = y;
        /* 【第9期·接线】原实现命中检测结果被丢弃（unused variable 'icon'）→
         *   图标 hover 高亮（draw_icon 的 highlighted 分支 + icons 层的 setter）
         *   永远不生效，用户不知道鼠标停在哪个图标上。
         *   现：悬停目标变化时交给 icons 层更新并刷新一次（不随每次移动重绘）。 */
        tui_icon_t *hover = tui_desktop_icons_hit_test(x, y);
        if (hover != g_last_hover_icon) {
            g_last_hover_icon = hover;
            tui_desktop_icons_handle_mouse(0, x, y);   /* 0 = 悬停/释放：只更新高亮 */
            tui_desktop_refresh(state);
        }
    }

    if (input->evtype == NCEV_BUTTON_PRESS) {
        g_events.mouse_clicked = 1;
        process_mouse_event(state, key);
        return 1;
    }

    if (input->evtype == NCEV_BUTTON_RELEASE) {
        if (g_events.drag_active) {
            g_events.drag_active = 0;
            g_events.drag_window = NULL;
            tui_desktop_refresh(state);
            return 1;
        }
        g_events.mouse_clicked = 0;
        return 0;
    }

    if (g_events.mouse_clicked && input->evtype == NCEV_MOTION) {
        tui_window_t *win = tui_window_get_focused();
        if (win && win->draggable) {
            g_events.drag_active = 1;
            g_events.drag_window = win;
            tui_window_update_drag(win, x, y);
            tui_desktop_refresh(state);
            return 1;
        }
    }

    return 0;
}

void tui_desktop_events_update_mouse_position(int x, int y) {
    g_events.mouse_x = x;
    g_events.mouse_y = y;
}

int tui_desktop_events_get_mouse_x(void) { return g_events.mouse_x; }
int tui_desktop_events_get_mouse_y(void) { return g_events.mouse_y; }
int tui_desktop_events_is_dragging(void) { return g_events.drag_active; }

void tui_desktop_events_cleanup(void) {
    memset(&g_events, 0, sizeof(g_events));
    LOG_DEBUG_T("TUIDesktopEvents", "Cleanup", "OK", "event system cleaned up");
}