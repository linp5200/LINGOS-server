/**
 * @file    src/install/install_model.h
 * @brief   模型下载（支持断点续传）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_MODEL_H
#define INSTALL_MODEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 下载模型
 * @param model_name 模型名称（"yolov8n" 或 "vosk-model-small-cn-0.22"）
 * @return 0 成功，-1 失败
 */
int install_model_download(const char *model_name);

/**
 * @brief 检查模型是否已下载且完整
 * @param model_name 模型名称
 * @return 1 已下载完整，0 未下载或不完整
 */
int install_model_is_ready(const char *model_name);

/**
 * @brief 检查所有模型是否就绪
 * @return 0 全部就绪，-1 有缺失
 */
int install_model_check_all(void);

/**
 * @brief 获取上次错误信息
 * @return 错误信息字符串
 */
const char* install_model_get_last_error(void);

/**
 * @brief 设置模型下载镜像源
 * @param mirror 镜像源 URL
 */
void install_model_set_mirror(const char *mirror);

/**
 * @brief 清空缓存
 */
void install_model_clear_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_MODEL_H */