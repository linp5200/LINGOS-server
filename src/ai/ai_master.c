/**
 * @file    ai_master.c
 * @brief   分类主AI（Code/Guard/General）简化实现
 * @version LN-B-5.0.0.0
 * @changes 修正 _current_lang 未声明、删除重复定义、使用动态名称数组
 */

#include "ai_master.h"
#include "common/string_no_sys.h"
#include "common/safe_string.h"
#include "common/lang.h"
#include "lib/log_extra.h"
#include "drivers/uart.h"
#include <string.h>

/* 外部引用语言变量（在 lang.h 中声明为 extern） */
extern int _current_lang;

/* 多语言名称数组 */
static const char *master_names_en[] = {
    "Code Master",
    "Guard Master",
    "General Master"
};
static const char *master_names_zh[] = {
    "代码主AI",
    "守卫主AI",
    "通用主AI"
};

/**
 * @brief 获取主AI类型名称（根据当前语言）
 */
const char *ai_master_type_name(master_ai_type_t type) {
    if (type > MASTER_GENERAL) return "unknown";
    const char **names = (_current_lang == 1) ? master_names_zh : master_names_en;
    return names[type];
}

void ai_master_init(void) {
    LOG_INFO_T("MasterAI", "Init", "Enter", "Initializing category master AIs");
    uart_puts(tr("[MasterAI] Initializing category master AIs...\n",
                 "[主AI] 正在初始化分类主AI...\n"));

    for (int i = 0; i <= MASTER_GENERAL; i++) {
        uart_puts(tr("[MasterAI]   ", "[主AI]   "));
        uart_puts(ai_master_type_name(i));
        uart_puts(tr(" ready.\n", " 已就绪。\n"));
        LOG_DEBUG_T("MasterAI", "Init", "Master", "%s ready", ai_master_type_name(i));
    }

    LOG_INFO_T("MasterAI", "Init", "OK", "Category master AIs initialized.");
    uart_puts(tr("[MasterAI] All category master AIs ready.\n",
                 "[主AI] 所有分类主AI已就绪。\n"));
}

int ai_master_review(master_ai_type_t master_type,
                     void *sub_result,
                     master_review_t *review) {
    (void)sub_result;

    LOG_DEBUG_T("MasterAI", "Review", "Enter", "master_type=%d", master_type);

    if (!review) {
        LOG_ERROR_T("MasterAI", "Review", "Invalid", "review is NULL");
        return -1;
    }

    review->approved = 1;
    review->needs_revision = 0;
    safe_strncpy(review->suggestion,
                 tr("Review passed (sub-AI system deprecated).",
                    "审查通过（子AI系统已弃用）。"),
                 sizeof(review->suggestion));

    LOG_INFO_T("MasterAI", "Review", "OK", "%s: APPROVED", ai_master_type_name(master_type));
    uart_puts(tr("[MasterAI] ", "[主AI] "));
    uart_puts(ai_master_type_name(master_type));
    uart_puts(tr(" review: APPROVED (simplified)\n",
                 " 审查：通过（简化模式）\n"));

    return 0;
}