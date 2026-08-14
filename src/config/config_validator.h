/**
 * @file    src/config/config_validator.h
 * @brief   配置验证函数声明
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_VALIDATOR_H
#define CONFIG_VALIDATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 验证 API Key 格式
 * @param value 要验证的值
 * @param err_msg 输出错误信息
 * @param err_size 错误信息缓冲区大小
 * @return 0 有效，-1 无效
 */
int config_validate_api_key(const char *value, char *err_msg, size_t err_size);

/**
 * @brief 验证 URL 格式
 * @param value 要验证的值
 * @param err_msg 输出错误信息
 * @param err_size 错误信息缓冲区大小
 * @return 0 有效，-1 无效
 */
int config_validate_url(const char *value, char *err_msg, size_t err_size);

/**
 * @brief 验证非空
 * @param value 要验证的值
 * @param err_msg 输出错误信息
 * @param err_size 错误信息缓冲区大小
 * @return 0 有效，-1 无效
 */
int config_validate_nonempty(const char *value, char *err_msg, size_t err_size);

/**
 * @brief 通用验证分发器
 * @param value 要验证的值
 * @param rule 验证规则（"api_key", "url", "nonempty"）
 * @param err_msg 输出错误信息
 * @param err_size 错误信息缓冲区大小
 * @return 0 有效，-1 无效
 */
int config_validate(const char *value, const char *rule, char *err_msg, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_VALIDATOR_H */