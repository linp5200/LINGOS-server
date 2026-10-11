/**
 * @file    src/config/wizard_engine.h
 * @brief   向导引擎接口定义
 * @version LN-0.4.3
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
/* ============================================================
 * 声明式配置项（先生 2026-10-01 拍板 · 16 字段 · 三端同源渲染基础）
 * ------------------------------------------------------------
 * 先生原话：「插件式——类别稳定 + 组内项声明式清单；插入即出现，无步骤号」。
 * 字段（16）：id / group / type / title双 / desc双 / options / default /
 *            advanced / skippable / secret / validate / validate_hint双 /
 *            requires / restart_required / order / since_version
 * 兼容：既有 13 字段结构**只增不改**——旧代码与内置 7 步零改动。
 * ============================================================ */
typedef struct wizard_step_def {
    /* ---- 既有字段（保持原样，旧调用方零改）---- */
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

    /* ---- 【2026-10-11】声明式清单新增字段（默认 0/空 = 旧行为）---- */
    char group[32];             /* 类别（稳定）：ai / security / network / runtime / ui / notify… */
    char desc_en[128];          /* 长描述（双语）——解释该项用途，非标题重复 */
    char desc_zh[128];
    int  advanced;              /* 1 = 高级配置区（与常规交互完全一致，仅分区） */
    int  skippable;             /* 1 = 非必要项，可直接跳过（区别于 optional：非快速模式也可跳） */
    int  secret;                /* 1 = 私密值（输入掩码 + 落盘走值级加密 api_key_enc） */
    char validate_hint_en[96];  /* 校验失败提示（双语） */
    char validate_hint_zh[96];
    char requires[64];          /* 条件显示表达式（例：ai.active == 'deepseek'） */
    int  restart_required;      /* 1 = 改动需重启生效（UI 提示） */
    int  order;                 /* 组内排序权重（小在前；0 = 未指定，按声明序） */
    char since_version[16];     /* 引入版本（未来迁移用） */
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

/* 【2026-10-02】交互式运行（完整循环——启动路径与 shell config 命令共用）
 * renderer_type: 0=TUI 1=CLI 2=RAW（见 config_renderer.h renderer_type_t）
 * force_override: 1=强制覆盖保存
 * @return 0=完成并保存 / 1=异常或保存失败 / 2=取消 / -1=初始化错误 */
int wizard_engine_run_interactive(wizard_engine_ctx_t *ctx, int renderer_type, int force_override);
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