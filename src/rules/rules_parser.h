/**
 * @file    rules_parser.h
 * @brief   规则解析头文件
 * @version LN-B-4.3.0.0
 */

#ifndef RULES_PARSER_H
#define RULES_PARSER_H

#include <stddef.h>

int rules_parser_generate_condition(const char *template_name, char *out, size_t out_len);
int rules_parser_generate_actions(const char **action_names, int count, char *out, size_t out_len);
int rules_parser_evaluate(const char *condition, int *result);
const char** rules_parser_get_condition_templates(void);
const char** rules_parser_get_action_templates(void);

#endif /* RULES_PARSER_H */