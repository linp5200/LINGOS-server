/**
 * @file    tui_desktop_render.c
 * @brief   TUI 桌面渲染（背景、状态栏、任务栏）
 * @version LN-B-4.2.0.0
 */

#include "tui_desktop.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 颜色定义
 * ============================================================ */

#define COLOR_BG_DESKTOP   0x1a1a2e
#define COLOR_BG_STATUS    0x0d0d1a
#define COLOR_BG_TASKBAR   0x16213e
#define COLOR_TEXT_STATUS  0x888888
#define COLOR_TEXT_TASKBAR 0xaaaaaa
#define COLOR_ACCENT       0x00ffff

/* ============================================================
 * 内部辅助：绘制水平线
 * ============================================================ */

static void draw_horizontal_line(struct ncplane *plane, int y, int x, int len, uint64_t color) {
    if (!plane || len <= 0) return;

    ncplane_set_fg_rgb(plane, color);
    ncplane_cursor_move_yx(plane, y, x);
    for (int i = 0; i < len; i++) {
        ncplane_putstr(plane, "─");
    }
}

/* ============================================================
 * 内部辅助：获取当前时间字符串
 * ============================================================ */

static void get_time_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, size, "%H:%M:%S", tm);
}

/* ============================================================
 * 公共渲染函数
 * ============================================================ */

void tui_desktop_render_background(tui_desktop_state_t *state) {
    if (!state || !state->desktop_plane) {
        LOG_WARN_T("TUIDesktopRender", "Background", "Invalid", "plane is NULL");
        return;
    }

    struct ncplane *plane = state->desktop_plane;
    int height = ncplane_dim_y(plane);
    int width = ncplane_dim_x(plane);

    /* 清除平面 */
    ncplane_erase(plane);

    /* 设置背景色 */
    ncplane_set_bg_rgb(plane, COLOR_BG_DESKTOP);
    ncplane_set_fg_rgb(plane, COLOR_ACCENT);

    /* 绘制简单的桌面背景装饰 */
    /* 左上角显示 LING OS 标识 */
    ncplane_cursor_move_yx(plane, 0, 1);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr(plane, "🖥️  LING OS Desktop");
    ncplane_set_styles(plane, 0);
    ncplane_set_fg_rgb(plane, 0x444444);

    /* 绘制一个简单的装饰框 */
    int box_y = 1;
    int box_x = 1;
    int box_w = 50;
    int box_h = 6;

    if (width > box_w + 2 && height > box_h + 2) {
        ncplane_set_fg_rgb(plane, 0x333355);
        /* 上边框 */
        ncplane_cursor_move_yx(plane, box_y, box_x);
        ncplane_putstr(plane, "╔");
        for (int i = 0; i < box_w - 2; i++) {
            ncplane_putstr(plane, "═");
        }
        ncplane_putstr(plane, "╗");

        /* 左右边框 */
        for (int i = 1; i < box_h - 1; i++) {
            ncplane_cursor_move_yx(plane, box_y + i, box_x);
            ncplane_putstr(plane, "║");
            ncplane_cursor_move_yx(plane, box_y + i, box_x + box_w - 1);
            ncplane_putstr(plane, "║");
        }

        /* 下边框 */
        ncplane_cursor_move_yx(plane, box_y + box_h - 1, box_x);
        ncplane_putstr(plane, "╚");
        for (int i = 0; i < box_w - 2; i++) {
            ncplane_putstr(plane, "═");
        }
        ncplane_putstr(plane, "╝");

        /* 中间显示欢迎信息 */
        ncplane_set_fg_rgb(plane, 0x8888cc);
        const char *welcome = tr("Welcome to LING OS", "欢迎使用 LING OS");
        int center_x = box_x + (box_w - strlen(welcome)) / 2;
        ncplane_cursor_move_yx(plane, box_y + 2, center_x);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr(plane, welcome);
        ncplane_set_styles(plane, 0);

        /* 子标题 */
        const char *sub = tr("Click icons to launch applications", "点击图标启动应用");
        center_x = box_x + (box_w - strlen(sub)) / 2;
        ncplane_cursor_move_yx(plane, box_y + 3, center_x);
        ncplane_set_fg_rgb(plane, 0x666688);
        ncplane_putstr(plane, sub);

        /* 版本信息 */
        char ver[32];
        safe_snprintf(ver, sizeof(ver), "vLN-B-4.2.0.0");
        center_x = box_x + (box_w - strlen(ver)) / 2;
        ncplane_cursor_move_yx(plane, box_y + 4, center_x);
        ncplane_set_fg_rgb(plane, 0x444466);
        ncplane_putstr(plane, ver);
    }

    /* 底部提示（只在小终端显示，避免和装饰框重叠） */
    if (height > 15) {
        ncplane_set_fg_rgb(plane, 0x333355);
        ncplane_cursor_move_yx(plane, height - 3, 2);
        ncplane_putstr(plane, "Press Q to exit desktop");
    }

    LOG_DEBUG_T("TUIDesktopRender", "Background", "OK", "rendered background (%dx%d)", width, height);
}

void tui_desktop_render_statusbar(tui_desktop_state_t *state) {
    if (!state || !state->status_plane) {
        LOG_WARN_T("TUIDesktopRender", "Statusbar", "Invalid", "plane is NULL");
        return;
    }

    struct ncplane *plane = state->status_plane;
    int width = ncplane_dim_x(plane);

    /* 清除 */
    ncplane_erase(plane);

    /* 设置背景 */
    ncplane_set_bg_rgb(plane, COLOR_BG_STATUS);
    ncplane_set_fg_rgb(plane, COLOR_TEXT_STATUS);

    /* 绘制分隔线 */
    draw_horizontal_line(plane, 0, 0, width, 0x333355);

    /* 状态信息 */
    ncplane_cursor_move_yx(plane, 0, 2);
    ncplane_set_fg_rgb(plane, COLOR_TEXT_STATUS);

    /* 左侧显示系统状态 */
    ncplane_putstr(plane, "🟢 LING OS");

    /* 中间显示内存信息（简单占位） */
    ncplane_set_fg_rgb(plane, 0x666688);
    int mid = width / 2;
    ncplane_cursor_move_yx(plane, 0, mid - 8);
    ncplane_putstr(plane, "● Memory: N/A");

    /* 右侧显示时间 */
    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    ncplane_set_fg_rgb(plane, 0x888899);
    ncplane_cursor_move_yx(plane, 0, width - 12);
    ncplane_putstr(plane, "⏰ ");
    ncplane_putstr(plane, time_str);

    LOG_DEBUG_T("TUIDesktopRender", "Statusbar", "OK", "rendered statusbar");
}

void tui_desktop_render_taskbar(tui_desktop_state_t *state) {
    if (!state || !state->taskbar_plane) {
        LOG_WARN_T("TUIDesktopRender", "Taskbar", "Invalid", "plane is NULL");
        return;
    }

    struct ncplane *plane = state->taskbar_plane;
    int width = ncplane_dim_x(plane);

    /* 清除 */
    ncplane_erase(plane);

    /* 设置背景 */
    ncplane_set_bg_rgb(plane, COLOR_BG_TASKBAR);

    /* 绘制分隔线 */
    ncplane_set_fg_rgb(plane, 0x2a2a4a);
    draw_horizontal_line(plane, 0, 0, width, 0x2a2a4a);

    /* 任务栏内容 */
    ncplane_cursor_move_yx(plane, 0, 2);
    ncplane_set_fg_rgb(plane, COLOR_TEXT_TASKBAR);
    ncplane_putstr(plane, "📌 ");

    /* 显示一些快捷入口（占位） */
    const char *items[] = {"Term", "Chat", "Files", "Config", "Help"};
    int x = 6;
    for (int i = 0; i < 5; i++) {
        if (x + 10 > width) break;
        ncplane_cursor_move_yx(plane, 0, x);
        ncplane_set_fg_rgb(plane, (i % 2 == 0) ? 0x88bbdd : 0x88ddbb);
        ncplane_putstr(plane, items[i]);
        x += 10;
        ncplane_set_fg_rgb(plane, 0x444466);
        if (i < 4) {
            ncplane_putstr(plane, "│");
            x += 2;
        }
    }

    /* 右侧显示提示 */
    ncplane_set_fg_rgb(plane, 0x444466);
    ncplane_cursor_move_yx(plane, 0, width - 20);
    ncplane_putstr(plane, "Q: exit  ↑↓: navigate");

    LOG_DEBUG_T("TUIDesktopRender", "Taskbar", "OK", "rendered taskbar");
}

void tui_desktop_render_icons(tui_desktop_state_t *state) {
    /* 图标渲染由 tui_desktop_icons.c 实现（批次20） */
    /* 此处为占位 */
    (void)state;
    LOG_DEBUG_T("TUIDesktopRender", "Icons", "Stub", "icons rendering not implemented yet");
}

void tui_desktop_render_windows(tui_desktop_state_t *state) {
    /* 窗口渲染由 tui_desktop_window.c 实现（批次20） */
    /* 此处为占位 */
    (void)state;
    LOG_DEBUG_T("TUIDesktopRender", "Windows", "Stub", "window rendering not implemented yet");
}