/**
 * @file    rules_executor.h
 * @brief   规则执行器头文件
 * @version LN-B-4.3.0.0
 */

#ifndef RULES_EXECUTOR_H
#define RULES_EXECUTOR_H

#include "rules_engine.h"

int rules_executor_run(const char actions[RULE_MAX_ACTIONS][RULE_ACTION_MAX], int count);

#endif /* RULES_EXECUTOR_H */