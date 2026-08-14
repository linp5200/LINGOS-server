/**
 * @file    rules_engine.h
 * @brief   规则引擎核心头文件
 * @version LN-B-4.3.0.0
 */

#ifndef RULES_ENGINE_H
#define RULES_ENGINE_H

#include <stdint.h>
#include <time.h>

#define RULE_NAME_MAX 64
#define RULE_CONDITION_MAX 256
#define RULE_ACTION_MAX 128
#define RULE_MAX_ACTIONS 8

typedef struct {
    char name[RULE_NAME_MAX];
    char condition[RULE_CONDITION_MAX];  /* IF 条件表达式 */
    char actions[RULE_MAX_ACTIONS][RULE_ACTION_MAX];  /* THEN 动作列表 */
    int action_count;
    int enabled;
    time_t created_at;
    time_t last_triggered;
    int trigger_count;
    int is_custom;  /* 1=开发者自定义表达式, 0=普通用户下拉框 */
} rule_t;

typedef struct {
    int enabled;
    int check_interval;  /* 检查间隔（秒） */
    int max_actions_per_rule;
    int require_ai_guard;
} rule_config_t;

int rules_engine_init(void);
int rules_engine_add(const rule_t *rule);
int rules_engine_remove(const char *name);
int rules_engine_list(rule_t *out, int max_count);
int rules_engine_check_and_execute(void);
const rule_config_t* rules_engine_get_config(void);
/**
 * @brief 加载规则引擎配置
 * @return 0 成功，-1 失败
 */
int rules_config_load(void);

#endif /* RULES_ENGINE_H */