/**
 * @file    src/install/install_system.h
 * @brief   系统包安装（多发行版支持）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_SYSTEM_H
#define INSTALL_SYSTEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 安装系统包
 * @param pkg_name 逻辑包名（如 "libmosquitto-dev"）
 * @return 0 成功，-1 失败
 */
int install_system_install(const char *pkg_name);

/**
 * @brief 检查系统包是否已安装
 * @param pkg_name 逻辑包名
 * @return 1 已安装，0 未安装
 */
int install_system_is_installed(const char *pkg_name);

/**
 * @brief 检查所有系统依赖是否满足
 * @return 0 全部满足，-1 有缺失
 */
int install_system_check_all(void);

/**
 * @brief 获取上次错误信息
 * @return 错误信息字符串
 */
const char* install_system_get_last_error(void);

/**
 * @brief 设置镜像源
 * @param mirror APT 镜像源 URL
 */
void install_system_set_mirror(const char *mirror);

/**
 * @brief 清空安装缓存
 */
void install_system_clear_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_SYSTEM_H */