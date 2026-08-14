/**
 * @file    rules_storage.h
 * @brief   规则存储头文件
 * @version LN-B-4.3.0.0
 */

#ifndef RULES_STORAGE_H
#define RULES_STORAGE_H

#include "rules_engine.h"

int rules_storage_save(const rule_t *rules, int count);
int rules_storage_load(rule_t *out, int max_count);

#endif /* RULES_STORAGE_H */