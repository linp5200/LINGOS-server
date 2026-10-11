/**
 * @file    dark_mode.h
 * @brief   暗影模式核心接口
 * @version LN-0.9.0-p5
 *
 * 【P5 接线上岗】dark_mode.c 的 4 个函数此前**全仓无声明**（仅 defense_mode.h 声明了
 *   shadow_mode 的两个函数）→ 链接存在但无人可调。本头文件补齐声明，
 *   由 security_wire.c 与 defense.c / defense_mode.c 真实调用。
 */

#ifndef SECURITY_DARK_MODE_H
#define SECURITY_DARK_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 暗影模式激活：生成临时密钥并标记敏感数据目录为加密态
 * @return 0 成功，-1 失败
 */
int dark_mode_encrypt_sensitive_data(void);

/**
 * @brief 暗影模式退出：清除加密标记与密钥
 * @return 0 成功，-1 未处于加密态
 */
int dark_mode_decrypt_data(void);

/**
 * @brief 该功能是否应被暗影/绝对保护模式阻止
 * @return 1 阻止，0 放行
 */
int dark_mode_should_block_feature(const char *feature);

/**
 * @brief 暗影模式下模拟硬件不可用（camera/microphone/gps）
 * @return -1 模拟不可用，0 正常
 */
int dark_mode_simulate_hardware_disable(const char *device);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_DARK_MODE_H */
