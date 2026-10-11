#ifndef SECURITY_OUTPUT_FILTER_H
#define SECURITY_OUTPUT_FILTER_H

#include <stdint.h>
#include <stddef.h>   /* size_t（可重入版签名用） */

/* 单线程便捷版：返回内部共享缓冲（下次调用即被覆盖，勿 free） */
const char *output_filter_redact(const char *output);

/* 【P5 接线】可重入版：结果拷入调用方缓冲，内部加锁。
 * audit.c（daemon 线程）与 security_wire.c（handler 线程）必须用这个版本。 */
int output_filter_redact_r(const char *output, char *out, size_t out_sz);

int output_filter_reload(void);

#endif