/**
 * @file    src/ui/startup_ui.h
 * @brief   启动界面 UI 辅助函数（进度显示、流式输出）
 * @version LN-B-5.1.2.6-rc
 * @changes 新增详细进度显示函数；集成 progress_bar 系统
 */

#ifndef UI_STARTUP_UI_H
#define UI_STARTUP_UI_H

#include "../lib/log_extra.h"  /* 复用进度状态 */
#include "progress_bar.h"      /* 进度条系统 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 原有函数声明
 * ============================================================ */

/**
 * @brief 显示依赖安装进度（动态刷新）
 * @param pkg_name  当前正在安装的包名
 * @param percent   进度百分比 (0-100)
 * @param status    进度状态（PROGRESS_RUNNING / DONE / FAILED）
 */
void ui_show_install_progress(const char *pkg_name, int percent, progress_status_t status);

/**
 * @brief 显示友好的网络错误提示
 * @param reason    错误原因（如 "DNS resolution failed"）
 */
void ui_show_network_error(const char *reason);

/**
 * @brief 显示一般性启动信息（流式输出）
 * @param fmt       格式字符串
 * @param ...       可变参数
 */
void ui_startup_message(const char *fmt, ...);

/**
 * @brief 显示当前启动步骤
 * @param step 步骤描述（如 "Creating directories..."）
 */
void ui_show_startup_step(const char *step);

/**
 * @brief 显示启动进度百分比
 * @param percent 进度百分比 (0-100)
 */
void ui_show_startup_progress(int percent);

/**
 * @brief 显示启动横幅（版本号、首次启动提示）
 */
void show_startup_banner(void);

/**
 * @brief 获取当前启动进度
 * @return 进度百分比
 */
int ui_get_startup_progress(void);

/**
 * @brief 获取当前启动步骤
 * @return 步骤描述字符串
 */
const char* ui_get_startup_step(void);

/* ============================================================
 * 新增：详细进度显示 (基于 progress_bar 系统)
 * ============================================================ */

/**
 * @brief 初始化详细进度
 * @param name 操作名称
 * @param type 操作类型 (progress_type_t)
 * @param total_items 总项目数
 * @param has_speed 是否有速度概念 (1=有, 0=无)
 */
void ui_init_detailed_progress(const char *name, progress_type_t type,
                               int total_items, int has_speed);

/**
 * @brief 更新详细进度
 * @param progress 当前进度 (0-100)
 * @param speed 当前速度 (MB/s, 无速度概念时传 0)
 * @param downloaded 已下载大小 (MB, 无速度概念时传 0)
 * @param total_size 总大小 (MB, 无速度概念时传 0)
 */
void ui_update_detailed_progress(int progress, double speed,
                                 double downloaded, double total_size);

/**
 * @brief 设置当前项目编号
 * @param current_item 当前项目编号 (从 1 开始)
 */
void ui_set_detailed_item(int current_item);

/**
 * @brief 完成详细进度
 * @param success 是否成功 (1=成功, 0=失败)
 * @param message 附加消息 (可为 NULL)
 */
void ui_finish_detailed_progress(int success, const char *message);

/**
 * @brief 重置详细进度 (用于下一个操作)
 */
void ui_reset_detailed_progress(void);

/**
 * @brief 获取当前详细进度上下文 (供 env_bootstrap 直接操作)
 * @return progress_ctx_t 指针
 */
progress_ctx_t* ui_get_progress_ctx(void);

/**
 * @brief 显示无速度概念步骤状态
 * @param step_name 步骤名称
 * @param status 状态 (OK/copying/setting up/making)
 * @param progress 进度百分比 (-1 表示不显示)
 */
void ui_show_step_status(const char *step_name, const char *status, int progress);

#ifdef __cplusplus
}
#endif

#endif /* UI_STARTUP_UI_H */