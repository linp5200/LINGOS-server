/**
 * @file    tui_app_launcher.h
 * @brief   TUI 桌面应用启动器头文件
 * @version LN-B-5.0.0.0
 */

#ifndef TUI_DESKTOP_TUI_APP_LAUNCHER_H
#define TUI_DESKTOP_TUI_APP_LAUNCHER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 各应用启动函数 */
void tui_app_launch_terminal(void);
void tui_app_launch_chat(void);
void tui_app_launch_files(void);
void tui_app_launch_monitor(void);
void tui_app_launch_config(void);
void tui_app_launch_help(void);

/**
 * @brief 根据名称运行应用
 * @param app_name 应用名称（terminal/chat/files/monitor/config/help）
 * @return 0 成功，-1 失败
 */
int tui_app_launcher_run(const char *app_name);

#ifdef __cplusplus
}
#endif

#endif /* TUI_DESKTOP_TUI_APP_LAUNCHER_H */