/**
 * @file    proot_detect.h
 * @brief   proot 环境检测
 * @version LN-B-3.8.0.0
 */

#ifndef DEBUG_PROOT_DETECT_H
#define DEBUG_PROOT_DETECT_H
#include <stddef.h>
/**
 * @brief 检测是否在 proot 环境中运行
 * @return 1 是 proot，0 否
 */
int is_in_proot(void);

/**
 * @brief 获取 proot 环境中的内核版本（安全读取）
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int get_proot_kernel_version(char *buf, size_t size);

/**
 * @brief 安全执行命令（proot 兼容）
 * @param cmd 命令
 * @param output 输出缓冲区
 * @param size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int safe_run_command(const char *cmd, char *output, size_t size);

#endif /* DEBUG_PROOT_DETECT_H */