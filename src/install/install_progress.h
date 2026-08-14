/**
 * @file    src/install/install_progress.h
 * @brief   安装进度显示与汇总
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_PROGRESS_H
#define INSTALL_PROGRESS_H

#include "install_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化进度显示
 * @param title 标题
 */
void install_progress_init(const char *title);

/**
 * @brief 设置当前阶段
 * @param stage_name 阶段名称
 * @param current 当前阶段编号
 * @param total 总阶段数
 */
void install_progress_set_stage(const char *stage_name, int current, int total);

/**
 * @brief 设置当前项目
 * @param item_name 项目名称
 * @param current 当前项目编号
 * @param total 总项目数
 */
void install_progress_set_item(const char *item_name, int current, int total);

/**
 * @brief 更新进度
 * @param progress 进度百分比 (0-100)
 * @param speed 速度 (MB/s)
 * @param downloaded 已下载 (MB)
 * @param total_size 总大小 (MB)
 * @param label 附加标签
 */
void install_progress_update(int progress, double speed, double downloaded,
                             double total_size, const char *label);

/**
 * @brief 完成当前项目的进度条
 * @param success 1=成功, 0=失败
 */
void install_progress_finish_item(int success);

/**
 * @brief 完成所有安装
 * @param summary 汇总结果
 */
void install_progress_finish(const install_summary_t *summary);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_PROGRESS_H */