/**
 * @file    tui_desktop_menu.c
 * @brief   TUI 桌面菜单（顶部菜单栏 + 右键上下文菜单）
 * @version LN-B-4.2.0.0
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "tui_desktop_icons.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "lang.h"
#include "uart.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 菜单项结构
 * ============================================================ */

typedef struct menu_item {
    char label[32];
    char shortcut[8];
    void (*action)(void);
    struct menu_item *next;
} menu_item_t;

/* ============================================================
 * 菜单结构
 * ============================================================ */

typedef struct {
    char name[32];
    menu_item_t *items;
    int item_count;
} menu_t;

/* ============================================================
 * 全局菜单状态
 * ============================================================ */

static menu_t g_top_menu[5];          /* 顶部菜单栏（最多5个） */
static int g_top_menu_count = 0;
static int g_menu_active = 0;
static int g_menu_selected = -1;
static menu_item_t *g_context_menu = NULL;  /* 右键菜单项 */

/* ============================================================
 * 内部辅助：添加菜单项
 * ============================================================ */

static void add_menu_item(menu_t *menu, const char *label, const char *shortcut,
                          void (*action)(void)) {
    if (!menu || !label) return;

    menu_item_t *item = calloc(1, sizeof(menu_item_t));
    if (!item) {
        LOG_ERROR_T("TUIDesktopMenu", "AddItem", "MallocFail", "calloc failed");
        return;
    }

    safe_strncpy(item->label, label, sizeof(item->label));
    if (shortcut) {
        safe_strncpy(item->shortcut, shortcut, sizeof(item->shortcut));
    }
    item->action = action;
    item->next = menu->items;
    menu->items = item;
    menu->item_count++;
}

/* ============================================================
 * 菜单动作函数
 * ============================================================ */

static void action_quit(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Quit", "quit desktop requested");
    tui_desktop_state_t *state = tui_desktop_get_state();
    if (state) {
        tui_desktop_set_exit(state, 0);
    }
}

static void action_about(void) {
    uart_puts(COLOR_CYAN);
    uart_puts("\n┌─────────────────────────────────────────────────────────────┐\n");
    uart_puts("│  LING OS Desktop v4.2.0.0                               │\n");
    uart_puts("│  AI-powered personal operating system                    │\n");
    uart_puts("│  Based on Notcurses TUI                                 │\n");
    uart_puts("└─────────────────────────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);
    LOG_INFO_T("TUIDesktopMenu", "Action", "About", "about displayed");
}

static void action_refresh(void) {
    tui_desktop_state_t *state = tui_desktop_get_state();
    if (state) {
        tui_desktop_refresh(state);
        LOG_DEBUG_T("TUIDesktopMenu", "Action", "Refresh", "desktop refreshed");
    }
}

static void action_terminal(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Terminal", "launching terminal");
    system("lingos_linux &");
}

static void action_chat(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Chat", "launching chat");
    system("lingos_linux -c 'nook chat' &");
}

static void action_files(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Files", "launching file manager");
    system("lingos_linux -c 'ls -la /LINGOS' &");
}

static void action_config(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Config", "launching config");
    system("lingos_linux -c 'system configuration' &");
}

static void action_system_health(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Health", "launching system health");
    system("lingos_linux -c 'system health' &");
}

static void action_apps(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Apps", "launching app list");
    system("lingos_linux -c 'app list' &");
}

static void action_help(void) {
    LOG_INFO_T("TUIDesktopMenu", "Action", "Help", "displaying help");
    uart_puts(COLOR_CYAN);
    uart_puts("\nAvailable desktop shortcuts:\n");
    uart_puts("  Q          - Quit desktop\n");
    uart_puts("  Alt+1..8   - Launch applications\n");
    uart_puts("  Mouse      - Click icons, drag windows\n");
    uart_puts("  Right-click - Context menu\n");
    uart_puts(COLOR_RESET);
}

/* ============================================================
 * 内部辅助：初始化顶部菜单
 * ============================================================ */

static void init_top_menu(void) {
    g_top_menu_count = 0;

    /* 文件菜单 */
    menu_t *file = &g_top_menu[g_top_menu_count++];
    safe_strncpy(file->name, "File", sizeof(file->name));
    add_menu_item(file, "Terminal", "Alt+1", action_terminal);
    add_menu_item(file, "Chat", "Alt+2", action_chat);
    add_menu_item(file, "Files", "Alt+3", action_files);
    add_menu_item(file, "Config", "Alt+4", action_config);
    add_menu_item(file, "---", NULL, NULL);
    add_menu_item(file, "Quit", "Alt+Q", action_quit);

    /* 编辑菜单 */
    menu_t *edit = &g_top_menu[g_top_menu_count++];
    safe_strncpy(edit->name, "Edit", sizeof(edit->name));
    add_menu_item(edit, "Refresh", "F5", action_refresh);

    /* 查看菜单 */
    menu_t *view = &g_top_menu[g_top_menu_count++];
    safe_strncpy(view->name, "View", sizeof(view->name));
    add_menu_item(view, "Health", "Alt+5", action_system_health);
    add_menu_item(view, "Apps", "Alt+6", action_apps);

    /* 工具菜单 */
    menu_t *tools = &g_top_menu[g_top_menu_count++];
    safe_strncpy(tools->name, "Tools", sizeof(tools->name));
    add_menu_item(tools, "Refresh", "F5", action_refresh);

    /* 帮助菜单 */
    menu_t *help = &g_top_menu[g_top_menu_count++];
    safe_strncpy(help->name, "Help", sizeof(help->name));
    add_menu_item(help, "About", NULL, action_about);
    add_menu_item(help, "Shortcuts", NULL, action_help);

    LOG_DEBUG_T("TUIDesktopMenu", "Init", "OK", "top menu initialized with %d menus", g_top_menu_count);
}

/* ============================================================
 * 内部辅助：初始化右键菜单
 * ============================================================ */

static void init_context_menu(void) {
    /* 清空现有菜单 */
    menu_item_t *item = g_context_menu;
    while (item) {
        menu_item_t *next = item->next;
        free(item);
        item = next;
    }
    g_context_menu = NULL;

    /* 添加右键菜单项 */
    void add_ctx_item(const char *label, void (*action)(void)) {
        menu_item_t *new_item = calloc(1, sizeof(menu_item_t));
        if (!new_item) return;
        safe_strncpy(new_item->label, label, sizeof(new_item->label));
        new_item->action = action;
        new_item->next = g_context_menu;
        g_context_menu = new_item;
    }

    add_ctx_item("Terminal", action_terminal);
    add_ctx_item("Chat", action_chat);
    add_ctx_item("Files", action_files);
    add_ctx_item("Refresh", action_refresh);
    add_ctx_item("---", NULL);
    add_ctx_item("About", action_about);
    add_ctx_item("Quit", action_quit);

    LOG_DEBUG_T("TUIDesktopMenu", "InitContext", "OK", "context menu initialized");
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void tui_desktop_menu_init(void) {
    LOG_INFO_T("TUIDesktopMenu", "Init", "Enter", "initializing desktop menu system");
    init_top_menu();
    init_context_menu();
    g_menu_active = 0;
    g_menu_selected = -1;
    LOG_INFO_T("TUIDesktopMenu", "Init", "OK", "menu system ready");
}

void tui_desktop_menu_render(struct ncplane *plane) {
    if (!plane) {
        LOG_WARN_T("TUIDesktopMenu", "Render", "Invalid", "plane is NULL");
        return;
    }

    /* 绘制顶部菜单栏 */
    int x = 2;
    ncplane_set_fg_rgb(plane, 0x888888);
    ncplane_set_bg_rgb(plane, 0x0d0d1a);

    for (int i = 0; i < g_top_menu_count; i++) {
        menu_t *menu = &g_top_menu[i];
        ncplane_cursor_move_yx(plane, 0, x);
        ncplane_set_fg_rgb(plane, (i == g_menu_selected) ? 0x00ccff : 0x888888);
        ncplane_set_styles(plane, (i == g_menu_selected) ? NCSTYLE_BOLD : 0);
        ncplane_putstr(plane, menu->name);
        ncplane_set_styles(plane, 0);
        x += strlen(menu->name) + 3;
    }

    /* 绘制分隔线 */
    int width = ncplane_dim_x(plane);
    ncplane_set_fg_rgb(plane, 0x333355);
    ncplane_cursor_move_yx(plane, 0, 0);
    for (int i = 0; i < width; i++) {
        ncplane_putstr(plane, "─");
    }
}

void tui_desktop_menu_show_top(int index) {
    if (index < 0 || index >= g_top_menu_count) {
        g_menu_active = 0;
        g_menu_selected = -1;
        return;
    }

    g_menu_active = 1;
    g_menu_selected = index;

    LOG_DEBUG_T("TUIDesktopMenu", "ShowTop", "OK", "menu %d selected", index);

    /* 显示菜单内容（简单实现：直接输出到终端） */
    menu_t *menu = &g_top_menu[index];
    uart_puts(COLOR_CYAN);
    uart_puts("\n┌───────────────────────────────────────────┐\n");
    uart_puts("│ ");
    uart_puts(menu->name);
    int padding = 40 - strlen(menu->name) - 2;
    for (int i = 0; i < padding; i++) uart_puts(" ");
    uart_puts("│\n");
    uart_puts("├───────────────────────────────────────────┤\n");

    menu_item_t *item = menu->items;
    int count = 1;
    while (item) {
        if (strcmp(item->label, "---") == 0) {
            uart_puts("├───────────────────────────────────────────┤\n");
        } else {
            uart_puts("│ ");
            uart_puts(item->label);
            int pad = 40 - strlen(item->label) - 2;
            for (int i = 0; i < pad; i++) uart_puts(" ");
            if (item->shortcut[0]) {
                uart_puts(item->shortcut);
            }
            uart_puts(" │\n");
        }
        item = item->next;
        count++;
    }
    uart_puts("└───────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);
}

void tui_desktop_menu_hide_top(void) {
    g_menu_active = 0;
    g_menu_selected = -1;
    LOG_DEBUG_T("TUIDesktopMenu", "HideTop", "OK", "menu hidden");
}

void tui_desktop_menu_show_context(int x, int y) {
    LOG_INFO_T("TUIDesktopMenu", "ShowContext", "Enter", "x=%d, y=%d", x, y);

    uart_puts(COLOR_CYAN);
    uart_puts("\n┌───────────────────────────────────────────┐\n");
    uart_puts("│           Context Menu                   │\n");
    uart_puts("├───────────────────────────────────────────┤\n");

    menu_item_t *item = g_context_menu;
    while (item) {
        if (strcmp(item->label, "---") == 0) {
            uart_puts("├───────────────────────────────────────────┤\n");
        } else {
            uart_puts("│ ");
            uart_puts(item->label);
            int pad = 40 - strlen(item->label) - 2;
            for (int i = 0; i < pad; i++) uart_puts(" ");
            uart_puts(" │\n");
        }
        item = item->next;
    }
    uart_puts("└───────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);
}

int tui_desktop_menu_handle_key(int key) {
    if (!g_menu_active) return 0;

    if (key == 27) { /* ESC */
        tui_desktop_menu_hide_top();
        return 1;
    }

    /* 数字键选择菜单项（简化） */
    if (key >= '1' && key <= '9') {
        int idx = key - '1';
        if (g_menu_selected >= 0 && g_menu_selected < g_top_menu_count) {
            menu_t *menu = &g_top_menu[g_menu_selected];
            menu_item_t *item = menu->items;
            int count = 0;
            while (item) {
                if (strcmp(item->label, "---") != 0) {
                    if (count == idx && item->action) {
                        item->action();
                        tui_desktop_menu_hide_top();
                        return 1;
                    }
                    count++;
                }
                item = item->next;
            }
        }
    }

    return 0;
}

void tui_desktop_menu_cleanup(void) {
    /* 清理顶部菜单 */
    for (int i = 0; i < g_top_menu_count; i++) {
        menu_item_t *item = g_top_menu[i].items;
        while (item) {
            menu_item_t *next = item->next;
            free(item);
            item = next;
        }
        g_top_menu[i].items = NULL;
        g_top_menu[i].item_count = 0;
    }
    g_top_menu_count = 0;

    /* 清理右键菜单 */
    menu_item_t *item = g_context_menu;
    while (item) {
        menu_item_t *next = item->next;
        free(item);
        item = next;
    }
    g_context_menu = NULL;

    LOG_DEBUG_T("TUIDesktopMenu", "Cleanup", "OK", "menu system cleaned up");
}