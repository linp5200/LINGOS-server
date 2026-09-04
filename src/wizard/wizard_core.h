/**
 * @file    src/wizard/wizard_core.h
 * @brief   配置向导核心上下文定义（补充缺失源码：wizard_context_t）
 * @version LN-0.4.3
 * @changes 补全缺失的 wizard_context_t 定义（UI 上下文 = 引擎上下文 + 渲染快照）
 */

#ifndef WIZARD_CORE_H
#define WIZARD_CORE_H

#include "../config/wizard_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 向导上下文（UI 层）
 * engine       — 向导引擎状态（步骤/栈/配置）
 * options      — 当前步骤选项快照（渲染用）
 * option_count — 选项数量
 * focus_index  — 键盘焦点索引
 * language     — "zh" / "en"
 * ============================================================ */
typedef struct wizard_context {
    wizard_engine_ctx_t engine;      /* 引擎上下文 */
    wizard_option_t options[16];     /* 当前步骤选项快照 */
    int option_count;
    int focus_index;
    char language[8];                /* "zh" / "en" */
} wizard_context_t;

#ifdef __cplusplus
}
#endif

#endif /* WIZARD_CORE_H */
