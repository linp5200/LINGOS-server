/**
 * @file    tui_controls.c
 * @brief   TUI 控件实现（按钮、列表、输入框）
 * @version LN-B-3.8.0.0
 */

#include "tui_controls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * 控件列表管理
 * ============================================================ */

tui_control_list_t* tui_controls_create_list(void) {
    tui_control_list_t *list = calloc(1, sizeof(tui_control_list_t));
    return list;
}

void tui_controls_destroy_list(tui_control_list_t *list) {
    if (!list) return;
    tui_control_t *cur = list->head;
    while (cur) {
        tui_control_t *next = cur->next;
        if (cur->label) free(cur->label);
        free(cur);
        cur = next;
    }
    free(list);
}

/* ============================================================
 * 控件创建
 * ============================================================ */

static tui_control_t* create_control(tui_control_list_t *list,
                                     tui_ctl_type_t type,
                                     const char *label,
                                     int x, int y, int width,
                                     void *data) {
    tui_control_t *ctl = calloc(1, sizeof(tui_control_t));
    if (!ctl) return NULL;

    ctl->type = type;
    if (label) {
        ctl->label = strdup(label);
    }
    ctl->x = x;
    ctl->y = y;
    ctl->width = width;
    ctl->height = 1;
    ctl->data = data;
    ctl->focused = 0;
    ctl->selected = 0;

    if (list->tail) {
        list->tail->next = ctl;
        ctl->prev = list->tail;
        list->tail = ctl;
    } else {
        list->head = list->tail = ctl;
    }
    list->count++;

    return ctl;
}

tui_control_t* tui_controls_add_button(tui_control_list_t *list,
                                       const char *label,
                                       int x, int y, int width,
                                       void (*on_click)(tui_control_t *, void *),
                                       void *user_data) {
    tui_control_t *ctl = create_control(list, CTL_BUTTON, label, x, y, width, NULL);
    if (ctl) {
        ctl->on_click = on_click;
        ctl->user_data = user_data;
    }
    return ctl;
}

tui_control_t* tui_controls_add_list_item(tui_control_list_t *list,
                                          const char *label,
                                          int x, int y,
                                          void *data) {
    return create_control(list, CTL_LIST_ITEM, label, x, y, 0, data);
}

tui_control_t* tui_controls_add_input(tui_control_list_t *list,
                                      const char *label,
                                      int x, int y, int width,
                                      char *buffer, int buffer_size) {
    tui_control_t *ctl = create_control(list, CTL_INPUT, label, x, y, width, buffer);
    if (ctl) {
        /* 存储缓冲区大小在 width 字段的负值中（不优雅，但简化） */
        /* 实际可以用一个包装结构，这里简单处理 */
        ctl->height = buffer_size;
    }
    return ctl;
}

tui_control_t* tui_controls_add_checkbox(tui_control_list_t *list,
                                         const char *label,
                                         int x, int y,
                                         int selected,
                                         void (*on_click)(tui_control_t *, void *),
                                         void *user_data) {
    tui_control_t *ctl = create_control(list, CTL_CHECKBOX, label, x, y, 0, NULL);
    if (ctl) {
        ctl->selected = selected;
        ctl->on_click = on_click;
        ctl->user_data = user_data;
    }
    return ctl;
}

tui_control_t* tui_controls_add_separator(tui_control_list_t *list,
                                          int x, int y, int width) {
    return create_control(list, CTL_SEPARATOR, "──────────────────", x, y, width, NULL);
}

/* ============================================================
 * 焦点管理
 * ============================================================ */

tui_control_t* tui_controls_focus_next(tui_control_list_t *list) {
    if (!list || list->count == 0) return NULL;
    if (!list->focused) {
        list->focused = list->head;
        return list->focused;
    }

    tui_control_t *cur = list->focused->next;
    while (cur) {
        if (cur->type != CTL_SEPARATOR) {
            list->focused->focused = 0;
            list->focused = cur;
            cur->focused = 1;
            return cur;
        }
        cur = cur->next;
    }

    /* 从头开始 */
    cur = list->head;
    while (cur) {
        if (cur->type != CTL_SEPARATOR) {
            list->focused->focused = 0;
            list->focused = cur;
            cur->focused = 1;
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

tui_control_t* tui_controls_focus_prev(tui_control_list_t *list) {
    if (!list || list->count == 0) return NULL;
    if (!list->focused) {
        list->focused = list->tail;
        return list->focused;
    }

    tui_control_t *cur = list->focused->prev;
    while (cur) {
        if (cur->type != CTL_SEPARATOR) {
            list->focused->focused = 0;
            list->focused = cur;
            cur->focused = 1;
            return cur;
        }
        cur = cur->prev;
    }

    /* 从尾部开始 */
    cur = list->tail;
    while (cur) {
        if (cur->type != CTL_SEPARATOR) {
            list->focused->focused = 0;
            list->focused = cur;
            cur->focused = 1;
            return cur;
        }
        cur = cur->prev;
    }
    return NULL;
}

/* ============================================================
 * 渲染
 * ============================================================ */

static void render_button(tui_control_t *ctl, struct ncplane *plane) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "[%s]", ctl->label);
    int padding = (ctl->width > len) ? (ctl->width - len) / 2 : 0;

    if (ctl->focused) {
        ncplane_set_fg_rgb(plane, 0x00ffff);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
    } else {
        ncplane_set_fg_rgb(plane, 0x888888);
        ncplane_set_styles(plane, 0);
    }

    ncplane_cursor_move_yx(plane, ctl->y, ctl->x + padding);
    ncplane_putstr(plane, buf);
}

static void render_list_item(tui_control_t *ctl, struct ncplane *plane) {
    if (ctl->focused) {
        ncplane_set_fg_rgb(plane, 0x00ffff);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_cursor_move_yx(plane, ctl->y, ctl->x);
        ncplane_putstr(plane, "◆ ");
    } else {
        ncplane_set_fg_rgb(plane, 0xcccccc);
        ncplane_set_styles(plane, 0);
        ncplane_cursor_move_yx(plane, ctl->y, ctl->x);
        ncplane_putstr(plane, "◇ ");
    }
    ncplane_putstr(plane, ctl->label);
}

static void render_input(tui_control_t *ctl, struct ncplane *plane) {
    char *buffer = (char*)ctl->data;
    int len = buffer ? strlen(buffer) : 0;

    if (ctl->focused) {
        ncplane_set_fg_rgb(plane, 0x00ffff);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
    } else {
        ncplane_set_fg_rgb(plane, 0x888888);
        ncplane_set_styles(plane, 0);
    }

    ncplane_cursor_move_yx(plane, ctl->y, ctl->x);
    ncplane_putstr(plane, ctl->label);
    ncplane_putstr(plane, ": ");

    if (buffer && len > 0) {
        ncplane_set_fg_rgb(plane, 0xffffff);
        ncplane_putstr(plane, buffer);
        if (ctl->focused) {
            ncplane_set_fg_rgb(plane, 0x00ffff);
            ncplane_putstr(plane, "█");
        }
    } else {
        ncplane_set_fg_rgb(plane, 0x444444);
        ncplane_putstr(plane, "(empty)");
        if (ctl->focused) {
            ncplane_set_fg_rgb(plane, 0x00ffff);
            ncplane_putstr(plane, "█");
        }
    }
}

static void render_checkbox(tui_control_t *ctl, struct ncplane *plane) {
    if (ctl->focused) {
        ncplane_set_fg_rgb(plane, 0x00ffff);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
    } else {
        ncplane_set_fg_rgb(plane, 0xcccccc);
        ncplane_set_styles(plane, 0);
    }

    ncplane_cursor_move_yx(plane, ctl->y, ctl->x);
    ncplane_putstr(plane, ctl->selected ? "[✓] " : "[ ] ");
    ncplane_putstr(plane, ctl->label);
}

static void render_separator(tui_control_t *ctl, struct ncplane *plane) {
    ncplane_set_fg_rgb(plane, 0x333333);
    ncplane_cursor_move_yx(plane, ctl->y, ctl->x);
    for (int i = 0; i < ctl->width && i < 80; i++) {
        ncplane_putchar(plane, 0x2500);
    }
}

void tui_controls_render(tui_control_list_t *list, struct ncplane *plane) {
    if (!list || !plane) return;

    tui_control_t *cur = list->head;
    while (cur) {
        switch (cur->type) {
            case CTL_BUTTON: render_button(cur, plane); break;
            case CTL_LIST_ITEM: render_list_item(cur, plane); break;
            case CTL_INPUT: render_input(cur, plane); break;
            case CTL_CHECKBOX: render_checkbox(cur, plane); break;
            case CTL_SEPARATOR: render_separator(cur, plane); break;
            default: break;
        }
        cur = cur->next;
    }
}

/* ============================================================
 * 事件处理
 * ============================================================ */

tui_control_t* tui_controls_handle_click(tui_control_list_t *list, int x, int y) {
    if (!list) return NULL;

    tui_control_t *cur = list->head;
    while (cur) {
        if (cur->type == CTL_BUTTON || cur->type == CTL_CHECKBOX) {
            if (x >= cur->x && x < cur->x + cur->width + 2 &&
                y >= cur->y && y < cur->y + cur->height) {
                if (cur->on_click) {
                    cur->on_click(cur, cur->user_data);
                }
                return cur;
            }
        }
        cur = cur->next;
    }
    return NULL;
}

int tui_controls_handle_key(tui_control_list_t *list, int key) {
    if (!list || !list->focused) return 0;

    tui_control_t *ctl = list->focused;

    if (key == 32 || key == 13) {  /* SPACE or ENTER */
        if (ctl->type == CTL_CHECKBOX) {
            ctl->selected = !ctl->selected;
            if (ctl->on_click) {
                ctl->on_click(ctl, ctl->user_data);
            }
            return 1;
        }
        if (ctl->type == CTL_BUTTON) {
            if (ctl->on_click) {
                ctl->on_click(ctl, ctl->user_data);
            }
            return 1;
        }
    }

    if (key == 9) {  /* TAB */
        tui_controls_focus_next(list);
        return 1;
    }

    /* 输入框处理 */
    if (ctl->type == CTL_INPUT) {
        char *buffer = (char*)ctl->data;
        int buf_size = ctl->height;
        int len = buffer ? strlen(buffer) : 0;

        if (key >= 32 && key <= 126 && len < buf_size - 1) {
            buffer[len] = key;
            buffer[len + 1] = '\0';
            return 1;
        }
        if (key == 127 || key == 8) {  /* BACKSPACE */
            if (len > 0) {
                buffer[len - 1] = '\0';
                return 1;
            }
        }
    }

    return 0;
}