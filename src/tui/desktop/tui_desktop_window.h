/**
 * @file    tui_desktop_window.h
 * @brief   TUI 桌面窗口管理器头文件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_WINDOW_H
#define TUI_DESKTOP_TUI_DESKTOP_WINDOW_H

#include <notcurses/notcurses.h>
#include <stdint.h>

/* ============================================================
 * 窗口结构
 * ============================================================ */

typedef struct tui_window {
    uint32_t id;                     /* 窗口唯一 ID */
    char title[64];                  /* 窗口标题 */
    struct ncplane *plane;           /* Notcurses 平面 */
    int x, y;                        /* 窗口位置 */
    int width, height;               /* 窗口尺寸 */
    int z_order;                     /* 层级顺序（越大越靠前） */
    int focused;                     /* 是否聚焦 (1/0) */
    int draggable;                   /* 是否可拖动 (1/0) */
    int is_dragging;                 /* 是否正在拖动 (1/0) */
    int drag_off_x, drag_off_y;      /* 拖动偏移量 */
    void (*on_close)(struct tui_window *win);   /* 关闭回调 */
    void (*on_resize)(struct tui_window *win);  /* 大小变化回调 */
    void *user_data;                 /* 用户数据 */
    struct tui_window *next;         /* 链表指针 */
} tui_window_t;

/* ============================================================
 * 窗口 API
 * ============================================================ */

/**
 * @brief 创建一个新窗口
 * @param title 窗口标题
 * @param x, y 起始坐标
 * @param width, height 宽高
 * @return 窗口指针，失败返回 NULL
 */
tui_window_t* tui_window_create(const char *title, int x, int y, int width, int height);

/**
 * @brief 销毁窗口
 * @param win 窗口指针
 */
void tui_window_destroy(tui_window_t *win);

/**
 * @brief 绘制窗口（边框、标题、关闭按钮）
 * @param win 窗口指针
 */
void tui_window_draw(tui_window_t *win);

/**
 * @brief 设置窗口标题
 * @param win 窗口指针
 * @param title 新标题
 */
void tui_window_set_title(tui_window_t *win, const char *title);

/**
 * @brief 聚焦窗口（置于顶层）
 * @param win 窗口指针
 */
void tui_window_focus(tui_window_t *win);

/**
 * @brief 处理窗口鼠标事件
 * @param win 窗口指针
 * @param event_type 事件类型（0=释放, 1=按下）
 * @param mouse_x, mouse_y 鼠标坐标（全局）
 * @return 1 事件已处理，0 未处理
 */
int tui_window_handle_mouse(tui_window_t *win, int event_type, int mouse_x, int mouse_y);

/**
 * @brief 更新窗口拖拽位置
 * @param win 窗口指针
 * @param mouse_x, mouse_y 当前鼠标坐标
 */
void tui_window_update_drag(tui_window_t *win, int mouse_x, int mouse_y);

/**
 * @brief 获取当前聚焦的窗口
 * @return 窗口指针，无聚焦返回 NULL
 */
tui_window_t* tui_window_get_focused(void);

/**
 * @brief 获取窗口链表头
 * @return 第一个窗口指针
 */
tui_window_t* tui_window_get_first(void);

/**
 * @brief 获取下一个窗口（用于遍历）
 * @param win 当前窗口
 * @return 下一个窗口指针
 */
static inline tui_window_t* tui_window_get_next(tui_window_t *win) {
    return win ? win->next : NULL;
}

/**
 * @brief 获取窗口数量
 * @return 窗口总数
 */
int tui_window_count(void);

/**
 * @brief 设置窗口关闭回调
 * @param win 窗口指针
 * @param on_close 回调函数
 */
void tui_window_set_on_close(tui_window_t *win, void (*on_close)(tui_window_t *));

/**
 * @brief 设置窗口用户数据
 * @param win 窗口指针
 * @param data 用户数据
 */
void tui_window_set_user_data(tui_window_t *win, void *data);

/**
 * @brief 获取窗口用户数据
 * @param win 窗口指针
 * @return 用户数据指针
 */
void* tui_window_get_user_data(tui_window_t *win);

#endif /* TUI_DESKTOP_TUI_DESKTOP_WINDOW_H */