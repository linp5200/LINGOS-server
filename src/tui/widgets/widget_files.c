/**
 * @file    src/tui/widgets/widget_files.c
 * @brief   TUI 桌面文件管理器小部件
 * @version LN-B-5.0.0.0
 * @changes 支持 NULL 参数使用默认路径；路径白名单限制；双文支持
 */

#include "widget_files.h"
#include "../desktop/tui_desktop.h"
#include "../desktop/tui_desktop_window.h"
#include "../../lib/log_extra.h"
#include "../../common/safe_string.h"
#include "../../common/lang.h"
#include "uart.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_FILES 64
#define MAX_PATH 512
#define PATH_WHITELIST "/LINGOS"

typedef struct {
    tui_window_t *window;
    char current_path[MAX_PATH];
    char files[MAX_FILES][128];
    int file_count;
    int selected_index;
    int active;
} files_state_t;

static files_state_t g_files;

static int is_path_allowed(const char *path) {
    if (!path) return 0;
    /* 仅允许 /LINGOS 和 /home 下的路径，防止访问系统敏感目录 */
    return (strstr(path, "/LINGOS") == path) ||
           (strstr(path, "/home") == path);
}

static int list_directory(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        LOG_WARN_T("WidgetFiles", "ListDir", "OpenFail", "cannot open %s", path);
        return -1;
    }

    g_files.file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && g_files.file_count < MAX_FILES) {
        if (entry->d_name[0] == '.') continue;
        safe_strncpy(g_files.files[g_files.file_count++], entry->d_name, sizeof(g_files.files[0]));
    }
    closedir(d);

    /* 按名称排序 */
    for (int i = 0; i < g_files.file_count - 1; i++) {
        for (int j = i + 1; j < g_files.file_count; j++) {
            if (strcmp(g_files.files[i], g_files.files[j]) > 0) {
                char tmp[128];
                safe_strncpy(tmp, g_files.files[i], sizeof(tmp));
                safe_strncpy(g_files.files[i], g_files.files[j], sizeof(g_files.files[0]));
                safe_strncpy(g_files.files[j], tmp, sizeof(g_files.files[0]));
            }
        }
    }
    return 0;
}

static void render_files(tui_window_t *win) {
    if (!win || !win->plane) return;

    struct ncplane *p = win->plane;
    int w = ncplane_dim_x(p);
    int h = ncplane_dim_y(p);

    ncplane_erase(p);

    ncplane_set_fg_rgb(p, 0x88ddff);
    ncplane_cursor_move_yx(p, 1, 2);
    ncplane_putstr(p, tr("📁 ", "📁 "));
    ncplane_putstr(p, g_files.current_path);

    int y = 3;
    for (int i = 0; i < g_files.file_count && y < h - 2; i++) {
        ncplane_cursor_move_yx(p, y, 2);
        if (i == g_files.selected_index) {
            ncplane_set_fg_rgb(p, 0x00ffcc);
            ncplane_set_styles(p, NCSTYLE_BOLD);
            ncplane_putstr(p, "> ");
        } else {
            ncplane_set_fg_rgb(p, 0x888888);
            ncplane_set_styles(p, 0);
            ncplane_putstr(p, "  ");
        }
        char full_path[MAX_PATH];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", g_files.current_path, g_files.files[i]);
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ncplane_set_fg_rgb(p, 0x88ddff);
        } else {
            ncplane_set_fg_rgb(p, 0xcccccc);
        }
        ncplane_putstr(p, g_files.files[i]);
        y++;
    }

    ncplane_set_fg_rgb(p, 0x444466);
    ncplane_cursor_move_yx(p, h - 2, 2);
    ncplane_putstr(p, tr("↑/↓: navigate  Enter: open  Q: close", "↑/↓: 导航  Enter: 打开  Q: 关闭"));

    tui_desktop_refresh(tui_desktop_get_state());
}

/* 【修改】支持 NULL 参数，使用默认路径 */
void widget_files_create(const char *path) {
    LOG_INFO_T("WidgetFiles", "Create", "Enter", "path='%s'", path ? path : "(null)");

    memset(&g_files, 0, sizeof(g_files));
    g_files.active = 1;
    g_files.selected_index = 0;

    if (path && *path && is_path_allowed(path)) {
        safe_strncpy(g_files.current_path, path, sizeof(g_files.current_path));
    } else {
        /* 默认使用 /LINGOS */
        safe_strncpy(g_files.current_path, "/LINGOS", sizeof(g_files.current_path));
    }

    g_files.window = tui_window_get_focused();
    if (!g_files.window) {
        LOG_WARN_T("WidgetFiles", "Create", "NoWindow", "no focused window");
        return;
    }

    if (list_directory(g_files.current_path) != 0) {
        safe_strncpy(g_files.current_path, "/LINGOS", sizeof(g_files.current_path));
        list_directory(g_files.current_path);
    }

    render_files(g_files.window);
    LOG_INFO_T("WidgetFiles", "Create", "OK", "file manager created at %s", g_files.current_path);
}

void widget_files_navigate(int direction) {
    if (!g_files.active) return;
    int new_idx = g_files.selected_index + direction;
    if (new_idx < 0) new_idx = 0;
    if (new_idx >= g_files.file_count) new_idx = g_files.file_count - 1;
    g_files.selected_index = new_idx;
    render_files(g_files.window);
}

void widget_files_open_selected(void) {
    if (!g_files.active || g_files.file_count == 0) return;

    const char *name = g_files.files[g_files.selected_index];
    char full_path[MAX_PATH];
    safe_snprintf(full_path, sizeof(full_path), "%s/%s", g_files.current_path, name);

    struct stat st;
    if (stat(full_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            if (!is_path_allowed(full_path)) {
                uart_puts(tr("Access denied to this directory.\n", "拒绝访问此目录。\n"));
                return;
            }
            safe_strncpy(g_files.current_path, full_path, sizeof(g_files.current_path));
            g_files.selected_index = 0;
            if (list_directory(g_files.current_path) == 0) {
                render_files(g_files.window);
                LOG_INFO_T("WidgetFiles", "Open", "Dir", "changed to %s", g_files.current_path);
            }
        } else {
            char info[256];
            safe_snprintf(info, sizeof(info), "File: %s (%lld bytes)", name, (long long)st.st_size);
            struct ncplane *p = g_files.window->plane;
            ncplane_set_fg_rgb(p, 0x88ff88);
            ncplane_cursor_move_yx(p, 1, 40);
            ncplane_putstr(p, info);
            tui_desktop_refresh(tui_desktop_get_state());
            LOG_INFO_T("WidgetFiles", "Open", "File", "%s", name);
        }
    }
}

void widget_files_destroy(void) {
    LOG_INFO_T("WidgetFiles", "Destroy", "Enter", "destroying file manager");
    g_files.active = 0;
    LOG_INFO_T("WidgetFiles", "Destroy", "OK", "file manager destroyed");
}