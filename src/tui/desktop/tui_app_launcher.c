/**
 * @file    src/tui/desktop/tui_app_launcher.c
 * @brief   TUI 桌面应用启动器
 * @version LN-B-5.0.0.0
 * @changes 修改文件管理器启动方式以适配 widget_files_create 签名；双文支持
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "tui_desktop_icons.h"
#include "widget_terminal.h"
#include "widget_chat.h"
#include "widget_files.h"
#include "widget_monitor.h"
#include "lang.h"
#include "safe_string.h"
#include "uart.h"
#include "log_extra.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void launch_app_window(const char *title, int width, int height,
                              void (*init_func)(void)) {
    tui_desktop_state_t *state = tui_desktop_get_state();
    if (!state) {
        LOG_ERROR_T("AppLauncher", "Launch", "NoState", "desktop state not available");
        return;
    }

    int win_w = width > 0 ? width : 60;
    int win_h = height > 0 ? height : 20;
    int x = (state->width - win_w) / 2;
    int y = (state->height - win_h) / 2;

    tui_window_t *win = tui_window_create(tr(title, title), x, y, win_w, win_h);
    if (!win) {
        LOG_ERROR_T("AppLauncher", "Launch", "WindowFail", "failed to create window");
        return;
    }

    tui_window_set_user_data(win, (void*)1);
    tui_desktop_refresh(state);

    if (init_func) {
        init_func();
    }
}

void tui_app_launch_terminal(void) {
    launch_app_window("Terminal", 70, 25, widget_terminal_create);
}

void tui_app_launch_chat(void) {
    launch_app_window("Nook Chat", 80, 28, widget_chat_create);
}

/* 【修改】文件管理器：先创建窗口，再手动调用 widget_files_create(NULL) */
void tui_app_launch_files(void) {
    launch_app_window("File Manager", 75, 30, NULL);
    /* 窗口已创建并聚焦，直接调用 widget_files_create 使用当前聚焦窗口 */
    widget_files_create(NULL);
}

void tui_app_launch_monitor(void) {
    launch_app_window("System Monitor", 70, 25, widget_monitor_create);
}

void tui_app_launch_config(void) {
    launch_app_window("Configuration", 70, 22, NULL);
    system("lingos_linux -c 'system configuration' &");
}

void tui_app_launch_help(void) {
    uart_puts(COLOR_CYAN);
    uart_puts(tr("\n=== LING OS Desktop Help ===\n", "\n=== LING OS 桌面帮助 ===\n"));
    uart_puts(tr("  Click icons to launch applications\n", "  点击图标启动应用\n"));
    uart_puts(tr("  Drag windows by title bar\n", "  拖动标题栏移动窗口\n"));
    uart_puts(tr("  Click [X] to close window\n", "  点击 [X] 关闭窗口\n"));
    uart_puts(tr("  Right-click for context menu\n", "  右键打开上下文菜单\n"));
    uart_puts(tr("  Press Q to exit desktop\n", "  按 Q 退出桌面\n"));
    uart_puts(COLOR_RESET);
}

int tui_app_launcher_run(const char *app_name) {
    LOG_INFO_T("AppLauncher", "Run", "Enter", "app_name='%s'", app_name ? app_name : "(null)");
    if (!app_name || !*app_name) return -1;

    if (strcmp(app_name, "terminal") == 0 || strcmp(app_name, "term") == 0) {
        tui_app_launch_terminal();
        return 0;
    } else if (strcmp(app_name, "chat") == 0) {
        tui_app_launch_chat();
        return 0;
    } else if (strcmp(app_name, "files") == 0) {
        tui_app_launch_files();
        return 0;
    } else if (strcmp(app_name, "monitor") == 0) {
        tui_app_launch_monitor();
        return 0;
    } else if (strcmp(app_name, "config") == 0) {
        tui_app_launch_config();
        return 0;
    } else if (strcmp(app_name, "help") == 0) {
        tui_app_launch_help();
        return 0;
    }

    LOG_WARN_T("AppLauncher", "Run", "Unknown", "unknown app: %s", app_name);
    return -1;
}