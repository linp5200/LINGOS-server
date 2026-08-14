/**
 * @file    widget_files.h
 * @brief   TUI 桌面文件管理器小部件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_WIDGETS_WIDGET_FILES_H
#define TUI_WIDGETS_WIDGET_FILES_H

/**
 * @brief 创建文件管理器小部件
 * @param path 起始路径（NULL 则使用 /LINGOS）
 */
void widget_files_create(const char *path);

/**
 * @brief 在文件列表中导航（上下移动）
 * @param direction 1 向下，-1 向上
 */
void widget_files_navigate(int direction);

/**
 * @brief 打开当前选中的文件或目录
 */
void widget_files_open_selected(void);

/**
 * @brief 销毁文件管理器小部件
 */
void widget_files_destroy(void);

#endif /* TUI_WIDGETS_WIDGET_FILES_H */