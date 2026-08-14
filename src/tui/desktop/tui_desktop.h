/**
 * @file    tui_desktop.h
 * @brief   TUI 桌面系统核心头文件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_H
#define TUI_DESKTOP_TUI_DESKTOP_H

#include <notcurses/notcurses.h>
#include <stdint.h>
#include <time.h>

/* ============================================================
 * 桌面配置结构
 * ============================================================ */

typedef struct {
    int enable_mouse;            /* 是否启用鼠标支持 (1/0) */
    int show_taskbar;            /* 是否显示任务栏 (1/0) */
    int show_statusbar;          /* 是否显示状态栏 (1/0) */
    int icon_size;               /* 图标大小（字符数） */
    int icon_spacing;            /* 图标间距 */
    int window_animation;        /* 窗口动画效果 (0=无, 1=简单) */
    char background[32];         /* 背景颜色或图案 */
} tui_desktop_config_t;

/* ============================================================
 * 桌面状态结构
 * ============================================================ */

typedef struct tui_desktop_state {
    struct notcurses *nc;        /* Notcurses 上下文 */
    struct ncplane *stdplane;    /* 标准平面 */
    struct ncplane *desktop_plane; /* 桌面背景平面 */
    struct ncplane *status_plane; /* 状态栏平面 */
    struct ncplane *taskbar_plane; /* 任务栏平面 */
    struct ncplane *menu_plane;   /* 菜单平面 */
    int width;                    /* 终端宽度 */
    int height;                   /* 终端高度 */
    int running;                  /* 是否运行中 */
    int exit_code;                /* 退出码 */
    tui_desktop_config_t config;  /* 配置 */
    void *window_manager;         /* 窗口管理器指针 */
    void *icon_manager;           /* 图标管理器指针 */
    void *menu_manager;           /* 菜单管理器指针 */
    void *user_data;              /* 用户数据 */
} tui_desktop_state_t;

/* ============================================================
 * 回调函数类型
 * ============================================================ */

typedef int (*tui_desktop_event_cb)(tui_desktop_state_t *state, int key, void *data);
typedef int (*tui_desktop_render_cb)(tui_desktop_state_t *state, void *data);

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 运行 TUI 桌面（主入口）
 * @return 0 正常退出，-1 错误退出
 */
int tui_desktop_run(void);

/**
 * @brief 初始化桌面系统
 * @param state 桌面状态结构
 * @return 0 成功，-1 失败
 */
int tui_desktop_init(tui_desktop_state_t *state);

/**
 * @brief 桌面主循环
 * @param state 桌面状态结构
 * @return 0 正常退出，-1 错误退出
 */
int tui_desktop_loop(tui_desktop_state_t *state);

/**
 * @brief 销毁桌面系统
 * @param state 桌面状态结构
 */
void tui_desktop_destroy(tui_desktop_state_t *state);

/**
 * @brief 刷新桌面（重新渲染所有平面）
 * @param state 桌面状态结构
 */
void tui_desktop_refresh(tui_desktop_state_t *state);

/**
 * @brief 检查是否应退出桌面
 * @param state 桌面状态结构
 * @return 1 应退出，0 继续运行
 */
int tui_desktop_should_exit(tui_desktop_state_t *state);

/**
 * @brief 设置桌面退出标志
 * @param state 桌面状态结构
 * @param exit_code 退出码
 */
void tui_desktop_set_exit(tui_desktop_state_t *state, int exit_code);

/**
 * @brief 获取桌面状态
 * @return 桌面状态指针
 */
tui_desktop_state_t* tui_desktop_get_state(void);

#endif /* TUI_DESKTOP_TUI_DESKTOP_H */