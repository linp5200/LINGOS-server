/**
 * @file    update_incremental.h
 * @brief   增量更新逻辑头文件
 * @version LN-B-4.3.0.0
 */

#ifndef UPDATE_INCREMENTAL_H
#define UPDATE_INCREMENTAL_H

#include <stddef.h>

int update_incremental_manifest(const char *base_dir, const char *target_dir,
                                char *out, size_t out_len);
int update_incremental_apply(const char *manifest_path);

#endif /* UPDATE_INCREMENTAL_H */