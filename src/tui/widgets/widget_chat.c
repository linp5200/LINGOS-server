/**
 * @file    src/tui/widgets/widget_chat.c
 * @brief   TUI 桌面 Nook 聊天小部件
 * @version LN-B-5.0.0.0
 * @changes 双文支持
 */

#include "widget_chat.h"
#include "../desktop/tui_desktop.h"
#include "../desktop/tui_desktop_window.h"
#include "../../ai/nook.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_HISTORY 20
#define MSG_BUF_SIZE 4096

typedef struct {
    tui_window_t *window;
    char history[MAX_HISTORY][256];
    int history_count;
    char input_buf[256];
    int input_len;
    int active;
} chat_state_t;

static chat_state_t g_chat;

static void render_chat(tui_window_t *win) {
    if (!win || !win->plane) return;

    struct ncplane *p = win->plane;
    int w = ncplane_dim_x(p);
    int h = ncplane_dim_y(p);

    ncplane_erase(p);

    int y = 2;
    int start = g_chat.history_count > MAX_HISTORY ? g_chat.history_count - MAX_HISTORY : 0;
    for (int i = start; i < g_chat.history_count && y < h - 3; i++) {
        ncplane_cursor_move_yx(p, y, 2);
        ncplane_set_fg_rgb(p, 0x88ddff);
        ncplane_putstr(p, g_chat.history[i]);
        y++;
    }

    ncplane_set_fg_rgb(p, 0x666688);
    ncplane_cursor_move_yx(p, h - 2, 2);
    ncplane_putstr(p, tr("> ", "> "));
    ncplane_set_fg_rgb(p, 0xffffff);
    ncplane_putstr(p, g_chat.input_buf);

    ncplane_set_fg_rgb(p, 0x88ff88);
    ncplane_putstr(p, "█");

    tui_desktop_refresh(tui_desktop_get_state());
}

static void send_to_nook(const char *msg) {
    LOG_INFO_T("WidgetChat", "Send", "Enter", "msg='%s'", msg);

    char response[4096];
    int ret = nook_ask_ollama(msg, NULL, response, sizeof(response), 60);

    char line[256];
    if (ret == 0) {
        safe_snprintf(line, sizeof(line), "Nook: %s", response);
    } else if (ret == -2) {
        safe_snprintf(line, sizeof(line), "Nook: [Timeout]");
    } else {
        safe_snprintf(line, sizeof(line), "Nook: [Service unavailable]");
    }

    if (g_chat.history_count < MAX_HISTORY) {
        safe_strncpy(g_chat.history[g_chat.history_count++], line, sizeof(g_chat.history[0]));
    } else {
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            memcpy(g_chat.history[i], g_chat.history[i+1], sizeof(g_chat.history[0]));
        }
        safe_strncpy(g_chat.history[MAX_HISTORY - 1], line, sizeof(g_chat.history[0]));
    }

    render_chat(g_chat.window);
}

void widget_chat_create(void) {
    LOG_INFO_T("WidgetChat", "Create", "Enter", "creating chat widget");

    memset(&g_chat, 0, sizeof(g_chat));
    g_chat.active = 1;

    g_chat.window = tui_window_get_focused();
    if (!g_chat.window) {
        LOG_WARN_T("WidgetChat", "Create", "NoWindow", "no focused window");
        return;
    }

    safe_strncpy(g_chat.history[0],
                 tr("Nook: Hello! I am Nook, your AI assistant.",
                    "Nook: 你好！我是 Nook，你的 AI 助手。"),
                 sizeof(g_chat.history[0]));
    g_chat.history_count = 1;

    render_chat(g_chat.window);
    LOG_INFO_T("WidgetChat", "Create", "OK", "chat widget created");
}

void widget_chat_input(const char *msg) {
    if (!g_chat.active || !msg || !*msg) return;

    LOG_INFO_T("WidgetChat", "Input", "Enter", "msg='%s'", msg);

    char line[256];
    safe_snprintf(line, sizeof(line), "You: %s", msg);
    if (g_chat.history_count < MAX_HISTORY) {
        safe_strncpy(g_chat.history[g_chat.history_count++], line, sizeof(g_chat.history[0]));
    } else {
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            memcpy(g_chat.history[i], g_chat.history[i+1], sizeof(g_chat.history[0]));
        }
        safe_strncpy(g_chat.history[MAX_HISTORY - 1], line, sizeof(g_chat.history[0]));
    }

    render_chat(g_chat.window);
    send_to_nook(msg);

    g_chat.input_buf[0] = '\0';
    g_chat.input_len = 0;
}

void widget_chat_keypress(int key) {
    if (!g_chat.active) return;

    if (key == 10 || key == 13) {
        if (g_chat.input_len > 0) {
            widget_chat_input(g_chat.input_buf);
        }
        return;
    }

    if (key == 127 || key == 8) {
        if (g_chat.input_len > 0) {
            g_chat.input_len--;
            g_chat.input_buf[g_chat.input_len] = '\0';
            render_chat(g_chat.window);
        }
        return;
    }

    if (key >= 32 && key <= 126 && g_chat.input_len < sizeof(g_chat.input_buf) - 1) {
        g_chat.input_buf[g_chat.input_len++] = (char)key;
        g_chat.input_buf[g_chat.input_len] = '\0';
        render_chat(g_chat.window);
    }
}

void widget_chat_destroy(void) {
    LOG_INFO_T("WidgetChat", "Destroy", "Enter", "destroying chat widget");
    g_chat.active = 0;
    LOG_INFO_T("WidgetChat", "Destroy", "OK", "chat widget destroyed");
}