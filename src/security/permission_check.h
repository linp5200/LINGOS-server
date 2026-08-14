/**
 * @file    permission_check.h
 * @brief   权限检查细化头文件
 * @version LN-B-4.3.0.0
 */

#ifndef SECURITY_PERMISSION_CHECK_H
#define SECURITY_PERMISSION_CHECK_H

#include <stddef.h>

int permission_check_file(const char *app_id, const char *path, int mode);
int permission_check_network(const char *app_id, const char *host, int port, int is_outgoing);
int permission_check_memory(const char *app_id, size_t size, int is_mmap);
int permission_check_cpu(const char *app_id, int cpu_percent, int priority);
int permission_check_device(const char *app_id, const char *device_type);
int permission_check_syscall(const char *app_id, int syscall_num);

#endif /* SECURITY_PERMISSION_CHECK_H */