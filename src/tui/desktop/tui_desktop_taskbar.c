/**
 * @file    tui_desktop_taskbar.c
 * @brief   TUI 桌面任务栏（显示窗口、切换聚焦）
 * @version LN-B-4.2.0.0
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 * 任务栏状态
 * ============================================================ */

typedef struct {
    int visible;
    int height;
    int width;
    int update_time;
    struct {
        int mem_usage;
        int task_count;
        int window_count;
        char time_str[16];
    } status;
} taskbar_state_t;

static taskbar_state_t g_taskbar;

/* ============================================================
 * 内部辅助：获取当前时间字符串
 * ============================================================ */

static void update_time_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, size, "%H:%M:%S", tm);
}

/* ============================================================
 * 内部辅助：获取窗口数量
 * ============================================================ */

static int get_window_count(void) {
    int count = 0;
    tui_window_t *win = tui_window_get_first();
    while (win) {
        count++;
        win = tui_window_get_next(win);
    }
    return count;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void tui_desktop_taskbar_init(void) {
    LOG_INFO_T("TUIDesktopTaskbar", "Init", "Enter", "initializing taskbar");

    g_taskbar.visible = 1;
    g_taskbar.height = 1;
    g_taskbar.width = 0;
    g_taskbar.update_time = 0;
    memset(&g_taskbar.status, 0, sizeof(g_taskbar.status));
    update_time_str(g_taskbar.status.time_str, sizeof(g_taskbar.status.time_str));
    g_taskbar.status.window_count = 0;
    g_taskbar.status.task_count = 0;

    LOG_INFO_T("TUIDesktopTaskbar", "Init", "OK", "taskbar initialized");
}

void tui_desktop_taskbar_render(tui_desktop_state_t *state) {
    if (!state || !state->taskbar_plane) {
        LOG_WARN_T("TUIDesktopTaskbar", "Render", "Invalid", "state or plane is NULL");
        return;
    }

    if (!g_taskbar.visible) return;

    struct ncplane *plane = state->taskbar_plane;
    int width = ncplane_dim_x(plane);

    /* 清除 */
    ncplane_erase(plane);

    /* 设置背景 */
    ncplane_set_bg_rgb(plane, 0x16213e);

    /* 绘制分隔线 */
    ncplane_set_fg_rgb(plane, 0x2a2a4a);
    ncplane_cursor_move_yx(plane, 0, 0);
    for (int i = 0; i < width; i++) {
        ncplane_putstr(plane, "─");
    }

    /* 左侧：快捷图标（占位） */
    ncplane_set_fg_rgb(plane, 0x88bbdd);
    ncplane_cursor_move_yx(plane, 0, 2);
    ncplane_putstr(plane, "📌 ");

    /* 中间：窗口列表 */
    int x = 8;
    tui_window_t *win = tui_window_get_first();
    int win_count = 0;
    while (win && x < width - 25) {
        char label[32];
        safe_snprintf(label, sizeof(label), "[%s]", win->title);
        if (x + strlen(label) > width - 25) break;

        ncplane_cursor_move_yx(plane, 0, x);
        ncplane_set_fg_rgb(plane, win->focused ? 0x00ccff : 0x666688);
        ncplane_set_styles(plane, win->focused ? NCSTYLE_BOLD : 0);
        ncplane_putstr(plane, label);
        ncplane_set_styles(plane, 0);
        x += strlen(label) + 2;
        win_count++;
        win = tui_window_get_next(win);
    }

    g_taskbar.status.window_count = win_count;

    /* 右侧：状态信息 */
    /* 窗口数量 */
    char status_buf[64];
    safe_snprintf(status_buf, sizeof(status_buf), "W:%d", win_count);
    ncplane_set_fg_rgb(plane, 0x666688);
    ncplane_cursor_move_yx(plane, 0, width - 22);
    ncplane_putstr(plane, status_buf);

    /* 内存状态（占位） */
    ncplane_cursor_move_yx(plane, 0, width - 16);
    ncplane_putstr(plane, "🟢");

    /* 时间 */
    char time_buf[16];
    update_time_str(time_buf, sizeof(time_buf));
    ncplane_set_fg_rgb(plane, 0x888899);
    ncplane_cursor_move_yx(plane, 0, width - 12);
    ncplane_putstr(plane, time_buf);

    /* 更新状态 */
    g_taskbar.width = width;
    g_taskbar.update_time = (int)time(NULL);

    LOG_DEBUG_T("TUIDesktopTaskbar", "Render", "OK", "taskbar rendered, %d windows", win_count);
}

void tui_desktop_taskbar_update(void) {
    g_taskbar.update_time = (int)time(NULL);
    update_time_str(g_taskbar.status.time_str, sizeof(g_taskbar.status.time_str));
    g_taskbar.status.window_count = get_window_count();

    tui_desktop_state_t *state = tui_desktop_get_state();
    if (state) {
        tui_desktop_refresh(state);
    }
}

int tui_desktop_taskbar_handle_mouse(int event_type, int mouse_x, int mouse_y) {
    if (!g_taskbar.visible) return 0;

    tui_desktop_state_t *state = tui_desktop_get_state();
    if (!state || !state->taskbar_plane) return 0;

    /* 检查鼠标是否在任务栏区域内 */
    int taskbar_y = state->height - g_taskbar.height;
    if (mouse_y < taskbar_y) return 0;

    if (event_type == 1) { /* 左键点击 */
        /* 遍历窗口，检查点击位置对应哪个窗口 */
        int x = 8;
        tui_window_t *win = tui_window_get_first();
        while (win && x < state->width - 25) {
            char label[32];
            safe_snprintf(label, sizeof(label), "[%s]", win->title);
            int len = strlen(label) + 2;
            if (mouse_x >= x && mouse_x < x + len) {
                /* 聚焦到该窗口 */
                tui_window_focus(win);
                LOG_DEBUG_T("TUIDesktopTaskbar", "Mouse", "Switch", "switched to window %s", win->title);
                return 1;
            }
            x += len;
            win = tui_window_get_next(win);
        }
    }

    return 0;
}

void tui_desktop_taskbar_set_visible(int visible) {
    g_taskbar.visible = visible;
    LOG_DEBUG_T("TUIDesktopTaskbar", "SetVisible", "%s", visible ? "ON" : "OFF");
}

int tui_desktop_taskbar_is_visible(void) {
    return g_taskbar.visible;
}

void tui_desktop_taskbar_cleanup(void) {
    memset(&g_taskbar, 0, sizeof(g_taskbar));
    LOG_DEBUG_T("TUIDesktopTaskbar", "Cleanup", "OK", "taskbar cleaned up");
}