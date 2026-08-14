/**
 * @file    rules_ai_guard.h
 * @brief   AI 守卫头文件
 * @version LN-B-4.3.0.0
 */

#ifndef RULES_AI_GUARD_H
#define RULES_AI_GUARD_H

#include "rules_engine.h"
#include <stddef.h>

int rules_ai_guard_check(const rule_t *rule, char *reason, size_t reason_len);

#endif /* RULES_AI_GUARD_H */