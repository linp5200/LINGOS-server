/**
 * @file    widget_monitor.h
 * @brief   TUI 桌面系统监控小部件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_WIDGETS_WIDGET_MONITOR_H
#define TUI_WIDGETS_WIDGET_MONITOR_H

/**
 * @brief 创建系统监控小部件（在聚焦窗口中显示实时系统状态）
 */
void widget_monitor_create(void);

/**
 * @brief 更新监控显示（刷新 CPU/内存/磁盘/服务状态）
 */
void widget_monitor_update(void);

/**
 * @brief 销毁监控小部件
 */
void widget_monitor_destroy(void);

#endif /* TUI_WIDGETS_WIDGET_MONITOR_H */