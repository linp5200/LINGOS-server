/**
 * @file    crash_dump.h
 * @brief   崩溃转储收集函数声明
 * @version LN-B-3.8.0.0
 */

#ifndef DEBUG_CRASH_DUMP_H
#define DEBUG_CRASH_DUMP_H

/**
 * @brief 收集系统完整转储（在信号处理子进程中调用）
 * @param signal_name 信号名称（如 "SIGSEGV"）
 * @param reason 崩溃原因描述
 * 
 * 收集内容：
 *   - 系统信息（CPU、内存、磁盘、进程列表、网络状态）
 *   - 日志文件（/LINGOS/Debug/*.log）
 *   - 配置文件（/LINGOS/system/config/*）
 *   - 根据错误类型判断是否包含用户数据
 *   - 打包为 /LINGOS/Dump/crash_<timestamp>.tar.gz
 */
void collect_system_dump(const char *signal_name, const char *reason);

#endif /* DEBUG_CRASH_DUMP_H */