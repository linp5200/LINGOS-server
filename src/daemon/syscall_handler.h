#ifndef SYSCALL_HANDLER_H
#define SYSCALL_HANDLER_H

#include <stdint.h>

/**
 * @brief 处理系统调用请求
 * @param operation  操作名称（如 "file_read"）
 * @param args_json  参数 JSON 字符串
 * @param out        输出缓冲区
 * @param out_len    缓冲区大小
 * @return 0 成功，-1 失败
 */
int handle_syscall(const char *operation, const char *args_json, char *out, uint32_t out_len);

#endif