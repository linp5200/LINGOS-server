#ifndef UPDATE_APPLY_CHANGES_H
#define UPDATE_APPLY_CHANGES_H

#include "manifest.h"

/**
 * @brief 应用更新包中的变更
 * @param extract_dir 解压临时目录
 * @param m 清单对象
 * @return 0 成功，-1 失败
 */
int apply_changes(const char *extract_dir, manifest_t *m);

#endif