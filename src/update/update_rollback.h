/**
 * @file    update_rollback.h
 * @brief   回滚版本管理头文件
 * @version LN-B-4.3.0.0
 */

#ifndef UPDATE_ROLLBACK_H
#define UPDATE_ROLLBACK_H

#include <stddef.h>

int update_rollback_create(const char *version);
int update_rollback_list(char *out, size_t out_len);
int update_rollback_apply(const char *version);

#endif /* UPDATE_ROLLBACK_H */