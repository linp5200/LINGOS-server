/**
 * @file    update_incremental_json.h
 * @brief   增量更新 JSON manifest 接口（2026-08-22 定稿）
 * @version LN-B-5.1.2.6-rc
 */

#ifndef UPDATE_INCREMENTAL_JSON_H
#define UPDATE_INCREMENTAL_JSON_H

#include <stddef.h>

/* 生成 JSON manifest（含 base_ver/target_ver/files[]） */
int update_incremental_json_manifest(const char *base_dir, const char *target_dir,
                                     const char *base_ver, const char *target_ver,
                                     char *out, size_t out_len);

/* 应用 JSON manifest：base_ver 匹配 + sha256 校验 + 备份 + 应用 */
/* current_ver 为空则跳过版本校验；返回 -2 = 版本不匹配，-1 = 失败，0 = 成功 */
int update_incremental_json_apply(const char *manifest_path, const char *target_root,
                                  const char *current_ver);

#endif /* UPDATE_INCREMENTAL_JSON_H */
