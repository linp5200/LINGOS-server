#ifndef UPDATE_SYSTEM_UPDATE_H
#define UPDATE_SYSTEM_UPDATE_H

#include <stdint.h>

/**
 * @brief 安装系统更新包（支持 .sub 内核包和 .latp 组件包）
 * @param pkg_path 包文件路径
 * @return 0 成功，-1 失败
 */
int system_update_install(const char *pkg_path);

/**
 * @brief 回滚到上一个内核版本
 * @return 0 成功，-1 失败
 */
int system_rollback(void);

#endif