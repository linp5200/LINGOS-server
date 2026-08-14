/**
 * @file    src/config/wizard_engine.h
 * @brief   向导引擎接口定义
 * @version LN-B-5.1.2.6-rc
 * @changes 移除了 wizard_config_t 定义（改用 config_core.h）
 *          添加 force_skip_verify 成员
 */

#ifndef WIZARD_ENGINE_H
#define WIZARD_ENGINE_H

#include "config_core.h"           /* 引入 wizard_config_t */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 步骤类型
 * ============================================================ */
typedef enum {
    STEP_TYPE_SELECT,
    STEP_TYPE_INPUT
} step_type_t;

/* ============================================================
 * 选项结构
 * ============================================================ */
typedef struct wizard_option {
    char id[32];
    char label_en[64];
    char label_zh[64];
    char value[128];
    char next_step[32];
    int is_disabled;
    int is_selected;
    int option_value;
} wizard_option_t;

/* ============================================================
 * 步骤定义
 * ============================================================ */
typedef struct wizard_step_def {
    char id[32];
    char title_en[64];
    char title_zh[64];
    step_type_t type;
    wizard_option_t options[16];
    int option_count;
    char input_prompt_en[128];
    char input_prompt_zh[128];
    char validate_rule[32];
    char default_value[64];
    char next_step[32];
    char parent_step[32];
    int optional;   /* 【先生要求】快速模式跳过（1=可选——完整模式才含） */
} wizard_step_def_t;

/* ============================================================
 * 渲染器上下文（前向声明）
 * ============================================================ */
struct renderer_ctx;

/* ============================================================
 * 向导引擎上下文
 * ============================================================ */
typedef struct wizard_engine_ctx {
    wizard_step_def_t *steps;
    int step_count;
    int current_index;
    int *stack;
    int stack_size;
    int stack_capacity;
    int cancelled;
    int renderer_type;
    struct renderer_ctx *renderer;
    wizard_config_t config;          /* 使用 config_core.h 中的定义 */
    int force_skip_verify;           /* 新增：用户选择强制跳过验证 */
    int wizard_mode;                 /* 【先生要求】0=快速（默认——必要项） 1=完整（所有可配置） */
} wizard_engine_ctx_t;

/* ============================================================
 * 向导引擎 API
 * ============================================================ */
int wizard_engine_init(wizard_engine_ctx_t *ctx, int renderer_type);
int wizard_engine_load_steps(wizard_engine_ctx_t *ctx);
int wizard_engine_run(wizard_engine_ctx_t *ctx);
int wizard_engine_handle_input(wizard_engine_ctx_t *ctx, const char *input);
wizard_step_def_t* wizard_engine_current_step(wizard_engine_ctx_t *ctx);
int wizard_engine_goto(wizard_engine_ctx_t *ctx, const char *step_id);
int wizard_engine_back(wizard_engine_ctx_t *ctx);
int wizard_engine_get_options(wizard_engine_ctx_t *ctx, wizard_option_t *options, int max_count);
const char* wizard_engine_get_input_prompt(wizard_engine_ctx_t *ctx);
int wizard_engine_set_value(wizard_engine_ctx_t *ctx, const char *option_id, const char *input_value);
int wizard_engine_save_config(wizard_engine_ctx_t *ctx);
void wizard_engine_set_mode(wizard_engine_ctx_t *ctx, int mode);  /* 【先生要求】0=快速 1=完整 */
void wizard_engine_set_force_skip(wizard_engine_ctx_t *ctx, int enable);

#ifdef __cplusplus
}
#endif

#endif /* WIZARD_ENGINE_H */