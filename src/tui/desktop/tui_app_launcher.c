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
#include <errno.h>   /* 【第9期】system() 失败原因的可见化 */

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

    /* 【第9期·双语修复】原为 tr(title, title)——两边同一字符串，等于没有双语；
     *   现由调用方传入 tr(EN, ZH) 的结果，此处直接使用。 */
    tui_window_t *win = tui_window_create(title, x, y, win_w, win_h);
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
    launch_app_window(tr("Terminal", "终端"), 70, 25, widget_terminal_create);
}

void tui_app_launch_chat(void) {
    launch_app_window(tr("Nook Chat", "诺克对话"), 80, 28, widget_chat_create);
}

/* 【修改】文件管理器：先创建窗口，再手动调用 widget_files_create(NULL) */
void tui_app_launch_files(void) {
    launch_app_window(tr("File Manager", "文件管理"), 75, 30, NULL);
    /* 窗口已创建并聚焦，直接调用 widget_files_create 使用当前聚焦窗口 */
    widget_files_create(NULL);
}

void tui_app_launch_monitor(void) {
    launch_app_window(tr("System Monitor", "系统监视"), 70, 25, widget_monitor_create);
}

void tui_app_launch_config(void) {
    launch_app_window(tr("Configuration", "配置"), 70, 22, NULL);

    /* 【第9期·禁静默失败】原实现直接 system("... &") 且不检查任何结果：
     *   lingos_linux 不存在时用户只看到一个空窗口、没有任何提示。
     *   现先探测可执行文件；缺失则明示原因与替代路径（不再假装已打开配置）。 */
    const char *cands[] = {
        "/usr/bin/lingos_linux", "/usr/local/bin/lingos_linux", "./lingos_linux", NULL
    };
    int found = 0;
    for (int i = 0; cands[i]; i++) {
        if (access(cands[i], X_OK) == 0) { found = 1; break; }
    }
    if (!found) {
        LOG_ERROR_T("AppLauncher", "Launch", "CliMissing",
                    "'lingos_linux' not found — configuration CLI cannot start "
                    "(searched /usr/bin, /usr/local/bin, ./)");
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("  Configuration CLI (lingos_linux) not found — window is empty.\n",
                     "  配置命令 lingos_linux 未找到——此窗口为空。\n"));
        uart_puts(tr("  Next: install lingos_linux, or run 'system configuration' in a terminal.\n",
                     "  下一步：安装 lingos_linux，或在终端执行 system configuration。\n"));
        uart_puts(COLOR_RESET);
        return;
    }
    int rc = system("lingos_linux -c 'system configuration' &");
    if (rc == -1) {
        LOG_ERROR_T("AppLauncher", "Launch", "SpawnFail", "cannot spawn configuration CLI (errno=%d)", errno);
        uart_puts(tr("  Failed to start configuration CLI — see log for details.\n",
                     "  配置命令启动失败——详情见日志。\n"));
    }
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