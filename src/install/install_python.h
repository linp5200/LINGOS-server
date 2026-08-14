/**
 * @file    src/install/install_python.h
 * @brief   Python 包安装（pipx → venv → pip）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef INSTALL_PYTHON_H
#define INSTALL_PYTHON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 安装 Python 包
 * @param module_name 模块名称
 * @return 0 成功，-1 失败
 */
int install_python_install(const char *module_name);

/**
 * @brief 检查 Python 模块是否可导入
 * @param module_name 模块名称
 * @return 1 可导入，0 不可导入
 */
int install_python_is_installed(const char *module_name);

/**
 * @brief 检查所有 Python 依赖是否满足
 * @return 0 全部满足，-1 有缺失
 */
int install_python_check_all(void);

/**
 * @brief 获取上次错误信息
 * @return 错误信息字符串
 */
const char* install_python_get_last_error(void);

/**
 * @brief 设置 PyPI 镜像源
 * @param mirror PyPI 镜像源 URL
 */
void install_python_set_mirror(const char *mirror);

/**
 * @brief 清空安装缓存
 */
void install_python_clear_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_PYTHON_H */