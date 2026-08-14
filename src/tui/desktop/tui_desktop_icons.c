/**
 * @file    src/tui/desktop/tui_desktop_icons.c
 * @brief   TUI 桌面图标管理（定义、绘制、点击处理）
 * @version LN-B-5.0.0.0
 * @changes 实现 tui_desktop_icons_cleanup；包含 tui_app_launcher.h；双文支持
 */

#include "tui_desktop.h"
#include "tui_desktop_window.h"
#include "tui_app_launcher.h"
#include "log_extra.h"
#include "safe_string.h"
#include "lang.h"
#include "uart.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ICON_LABEL 32
#define MAX_ICON_CMD 256

typedef struct tui_icon {
    char label[MAX_ICON_LABEL];
    char icon_char[8];
    char cmd[MAX_ICON_CMD];
    int x, y;
    int width, height;
    int highlighted;
    void (*on_click)(struct tui_icon *icon);
    void *user_data;
    struct tui_icon *next;
} tui_icon_t;

static tui_icon_t *g_icons = NULL;
static int g_icon_count = 0;

/* 预置应用定义 */
typedef struct {
    const char *label_en;
    const char *label_zh;
    const char *icon;
    const char *cmd;
    void (*launch)(void);
} preset_icon_t;

static const preset_icon_t preset_icons[] = {
    {"Terminal", "终端", "⌨️", "terminal", tui_app_launch_terminal},
    {"Chat", "聊天", "💬", "chat", tui_app_launch_chat},
    {"Files", "文件", "📁", "files", tui_app_launch_files},
    {"Monitor", "监控", "📊", "monitor", tui_app_launch_monitor},
    {"Settings", "设置", "⚙️", "config", tui_app_launch_config},
    {"Help", "帮助", "❓", "help", tui_app_launch_help}
};
#define PRESET_COUNT (sizeof(preset_icons) / sizeof(preset_icon_t))

static tui_icon_t* create_icon(const char *label, const char *icon_char,
                               const char *cmd, int x, int y) {
    tui_icon_t *icon = calloc(1, sizeof(tui_icon_t));
    if (!icon) return NULL;
    safe_strncpy(icon->label, label, sizeof(icon->label));
    safe_strncpy(icon->icon_char, icon_char ? icon_char : "📄", sizeof(icon->icon_char));
    safe_strncpy(icon->cmd, cmd ? cmd : "", sizeof(icon->cmd));
    icon->x = x;
    icon->y = y;
    icon->width = 10;
    icon->height = 4;
    icon->highlighted = 0;
    icon->on_click = NULL;
    icon->user_data = NULL;
    icon->next = NULL;
    return icon;
}

static void draw_icon(struct ncplane *plane, tui_icon_t *icon) {
    if (!plane || !icon) return;
    int x = icon->x;
    int y = icon->y;
    int w = icon->width;

    if (icon->highlighted) {
        ncplane_set_bg_rgb(plane, 0x2a2a4a);
        ncplane_set_fg_rgb(plane, 0xffffff);
    } else {
        ncplane_set_bg_rgb(plane, 0x1a1a2e);
        ncplane_set_fg_rgb(plane, 0xaaaaaa);
    }

    ncplane_cursor_move_yx(plane, y, x + (w - strlen(icon->icon_char)) / 2);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr(plane, icon->icon_char);
    ncplane_set_styles(plane, 0);

    int label_len = strlen(icon->label);
    if (label_len > w) label_len = w;
    int label_x = x + (w - label_len) / 2;
    ncplane_cursor_move_yx(plane, y + 1, label_x);

    if (icon->highlighted) {
        ncplane_set_fg_rgb(plane, 0x88ddff);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
    } else {
        ncplane_set_fg_rgb(plane, 0x888888);
        ncplane_set_styles(plane, 0);
    }
    ncplane_putstr(plane, icon->label);
    ncplane_set_bg_rgb(plane, 0x1a1a2e);
}

static void default_icon_click(tui_icon_t *icon) {
    if (!icon || !icon->cmd[0]) return;
    LOG_INFO_T("TUIDesktopIcons", "Click", "Execute", "cmd='%s'", icon->cmd);
    tui_app_launcher_run(icon->cmd);
}

void tui_desktop_icons_init(void) {
    LOG_INFO_T("TUIDesktopIcons", "Init", "Enter", "initializing desktop icons");

    tui_icon_t *icon = g_icons;
    while (icon) {
        tui_icon_t *next = icon->next;
        free(icon);
        icon = next;
    }
    g_icons = NULL;
    g_icon_count = 0;

    int cols = 5;
    int spacing_x = 12;
    int spacing_y = 5;
    int start_x = 4;
    int start_y = 10;

    for (int i = 0; i < PRESET_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = start_x + col * spacing_x;
        int y = start_y + row * spacing_y;
        const char *label = tr(preset_icons[i].label_en, preset_icons[i].label_zh);
        tui_icon_t *new_icon = create_icon(label, preset_icons[i].icon,
                                           preset_icons[i].cmd, x, y);
        if (new_icon) {
            new_icon->on_click = default_icon_click;
            new_icon->next = g_icons;
            g_icons = new_icon;
            g_icon_count++;
        }
    }
    LOG_INFO_T("TUIDesktopIcons", "Init", "OK", "created %d icons", g_icon_count);
}

void tui_desktop_icons_render(tui_desktop_state_t *state) {
    if (!state || !state->desktop_plane) return;
    struct ncplane *plane = state->desktop_plane;
    tui_icon_t *icon = g_icons;
    while (icon) {
        draw_icon(plane, icon);
        icon = icon->next;
    }
}

tui_icon_t* tui_desktop_icons_hit_test(int mouse_x, int mouse_y) {
    tui_icon_t *icon = g_icons;
    while (icon) {
        if (mouse_x >= icon->x && mouse_x < icon->x + icon->width &&
            mouse_y >= icon->y && mouse_y < icon->y + icon->height) {
            return icon;
        }
        icon = icon->next;
    }
    return NULL;
}

int tui_desktop_icons_handle_mouse(int event_type, int mouse_x, int mouse_y) {
    tui_icon_t *icon = tui_desktop_icons_hit_test(mouse_x, mouse_y);

    tui_icon_t *iter = g_icons;
    while (iter) {
        int prev_highlight = iter->highlighted;
        iter->highlighted = (iter == icon) ? 1 : 0;
        if (prev_highlight != iter->highlighted) {
            tui_desktop_state_t *state = tui_desktop_get_state();
            if (state && state->desktop_plane) {
                draw_icon(state->desktop_plane, iter);
            }
        }
        iter = iter->next;
    }

    if (icon && event_type == 1) {
        LOG_INFO_T("TUIDesktopIcons", "Mouse", "Click", "icon '%s' clicked", icon->label);
        if (icon->on_click) icon->on_click(icon);
        return 1;
    }
    return 0;
}

int tui_desktop_icons_count(void) { return g_icon_count; }

void tui_desktop_icons_add(const char *label, const char *icon_char,
                           const char *cmd, int x, int y) {
    if (!label || !*label) return;
    tui_icon_t *new_icon = create_icon(label, icon_char, cmd, x, y);
    if (new_icon) {
        new_icon->on_click = default_icon_click;
        new_icon->next = g_icons;
        g_icons = new_icon;
        g_icon_count++;
        LOG_INFO_T("TUIDesktopIcons", "Add", "OK", "icon '%s' added", label);
        tui_desktop_refresh(tui_desktop_get_state());
    }
}

void tui_desktop_icons_remove(const char *label) {
    if (!label) return;
    tui_icon_t **pp = &g_icons;
    while (*pp) {
        if (strcmp((*pp)->label, label) == 0) {
            tui_icon_t *to_remove = *pp;
            *pp = to_remove->next;
            free(to_remove);
            g_icon_count--;
            LOG_INFO_T("TUIDesktopIcons", "Remove", "OK", "icon '%s' removed", label);
            tui_desktop_refresh(tui_desktop_get_state());
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ============================================================
 * 新增：清理所有图标
 * ============================================================ */
void tui_desktop_icons_cleanup(void) {
    LOG_INFO_T("TUIDesktopIcons", "Cleanup", "Enter", "cleaning up icons");

    tui_icon_t *icon = g_icons;
    while (icon) {
        tui_icon_t *next = icon->next;
        free(icon);
        icon = next;
    }
    g_icons = NULL;
    g_icon_count = 0;

    LOG_INFO_T("TUIDesktopIcons", "Cleanup", "OK", "icons cleaned up");
}