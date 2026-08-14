/**
 * @file    src/tui/widgets/widget_monitor.c
 * @brief   TUI 桌面系统监控小部件
 * @version LN-B-5.0.0.0
 * @changes 双文支持
 */

#include "widget_monitor.h"
#include "../desktop/tui_desktop.h"
#include "../desktop/tui_desktop_window.h"
#include "../../health/system_health.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include "../../common/data_path.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    tui_window_t *window;
    int active;
    int update_interval;
} monitor_state_t;

static monitor_state_t g_monitor;

static void draw_progress_bar(struct ncplane *p, int y, int x, int width,
                              int percent, uint64_t color) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int filled = (percent * width) / 100;
    ncplane_cursor_move_yx(p, y, x);
    ncplane_set_fg_rgb(p, color);
    ncplane_putstr(p, "[");
    for (int i = 0; i < width; i++) {
        if (i < filled) ncplane_putstr(p, "█");
        else ncplane_putstr(p, "·");
    }
    ncplane_putstr(p, "]");
    char buf[16];
    safe_snprintf(buf, sizeof(buf), " %3d%%", percent);
    ncplane_putstr(p, buf);
}

static void update_monitor(tui_window_t *win) {
    if (!win || !win->plane) return;

    struct ncplane *p = win->plane;

    int mem = get_memory_usage();
    const char *root = lingos_data_root();
    int disk = get_disk_usage(root);
    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);
    int python_ok = check_python();
    int ai_ok = check_ai_backend();
    int net_ok = check_network();

    ncplane_erase(p);

    int y = 2;
    ncplane_set_fg_rgb(p, 0x88ddff);
    ncplane_set_styles(p, NCSTYLE_BOLD);
    ncplane_cursor_move_yx(p, y, 2);
    ncplane_putstr(p, tr("📊 System Monitor", "📊 系统监控"));
    ncplane_set_styles(p, 0);
    y += 2;

    ncplane_set_fg_rgb(p, 0x888888);
    ncplane_cursor_move_yx(p, y, 2);
    ncplane_putstr(p, tr("Memory:", "内存："));
    draw_progress_bar(p, y, 12, 40, mem, mem > 90 ? 0xff4444 : 0x88ddff);
    y += 2;

    ncplane_cursor_move_yx(p, y, 2);
    ncplane_putstr(p, tr("Disk:  ", "磁盘："));
    draw_progress_bar(p, y, 12, 40, disk, disk > 85 ? 0xff8844 : 0x88ddff);
    y += 2;

    ncplane_cursor_move_yx(p, y, 2);
    ncplane_set_fg_rgb(p, 0x888888);
    ncplane_putstr(p, tr("Load:  ", "负载："));
    char load_buf[32];
    safe_snprintf(load_buf, sizeof(load_buf), "%.2f, %.2f, %.2f", load1, load5, load15);
    ncplane_set_fg_rgb(p, 0x88ddff);
    ncplane_putstr(p, load_buf);
    if (load1 > 2.0) {
        ncplane_set_fg_rgb(p, 0xff8844);
        ncplane_putstr(p, " ⚠");
    }
    y += 2;

    ncplane_set_fg_rgb(p, 0x888888);
    ncplane_cursor_move_yx(p, y, 2);
    ncplane_putstr(p, tr("Services:", "服务："));
    y++;

    ncplane_cursor_move_yx(p, y, 4);
    ncplane_set_fg_rgb(p, python_ok ? 0x88ff88 : 0xff4444);
    ncplane_putstr(p, python_ok ? "✅ Python" : "❌ Python");
    ncplane_cursor_move_yx(p, y, 18);
    ncplane_set_fg_rgb(p, ai_ok ? 0x88ff88 : 0xff4444);
    ncplane_putstr(p, ai_ok ? "✅ AI Backend" : "❌ AI Backend");
    ncplane_cursor_move_yx(p, y, 38);
    ncplane_set_fg_rgb(p, net_ok ? 0x88ff88 : 0xff4444);
    ncplane_putstr(p, net_ok ? "✅ Network" : "❌ Network");
    y += 2;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "🕐 %Y-%m-%d %H:%M:%S", tm);
    ncplane_set_fg_rgb(p, 0x444466);
    ncplane_cursor_move_yx(p, y + 1, 2);
    ncplane_putstr(p, time_buf);

    ncplane_set_fg_rgb(p, 0x444466);
    ncplane_cursor_move_yx(p, ncplane_dim_y(p) - 2, 2);
    ncplane_putstr(p, tr("Auto-updates every 2 seconds", "每 2 秒自动更新"));

    tui_desktop_refresh(tui_desktop_get_state());
}

void widget_monitor_create(void) {
    LOG_INFO_T("WidgetMonitor", "Create", "Enter", "creating monitor widget");

    memset(&g_monitor, 0, sizeof(g_monitor));
    g_monitor.active = 1;
    g_monitor.update_interval = 2;

    g_monitor.window = tui_window_get_focused();
    if (!g_monitor.window) {
        LOG_WARN_T("WidgetMonitor", "Create", "NoWindow", "no focused window");
        return;
    }

    update_monitor(g_monitor.window);
    LOG_INFO_T("WidgetMonitor", "Create", "OK", "monitor widget created");
}

void widget_monitor_update(void) {
    if (!g_monitor.active || !g_monitor.window) return;
    update_monitor(g_monitor.window);
}

void widget_monitor_destroy(void) {
    LOG_INFO_T("WidgetMonitor", "Destroy", "Enter", "destroying monitor widget");
    g_monitor.active = 0;
    LOG_INFO_T("WidgetMonitor", "Destroy", "OK", "monitor widget destroyed");
}