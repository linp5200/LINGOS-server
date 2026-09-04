/**
 * @file    src/install/install_config.h
 * @brief   镜像源配置加载
 * @version LN-0.4.3
 * @par     核心协议：C1, CM
 */

#ifndef INSTALL_CONFIG_H
#define INSTALL_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 加载镜像源配置
 * @param apt_mirror 输出 APT 镜像源 URL（至少 256 字节）
 * @param pypi_mirror 输出 PyPI 镜像源 URL（至少 256 字节）
 * @return 0 成功，-1 失败
 */
int install_config_load_mirrors(char *apt_mirror, char *pypi_mirror);

/**
 * @brief 保存镜像源配置
 * @param apt_mirror APT 镜像源 URL（可为 NULL 表示不保存）
 * @param pypi_mirror PyPI 镜像源 URL（可为 NULL 表示不保存）
 * @return 0 成功，-1 失败
 */
int install_config_save_mirrors(const char *apt_mirror, const char *pypi_mirror);

/**
 * @brief 获取默认镜像源
 * @param apt_mirror 输出 APT 镜像源 URL（至少 256 字节）
 * @param pypi_mirror 输出 PyPI 镜像源 URL（至少 256 字节）
 */
void install_config_get_defaults(char *apt_mirror, char *pypi_mirror);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_CONFIG_H */