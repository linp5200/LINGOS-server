/**
 * @file    nook_personality.c
 * @brief   Nook 人格配置文件（内置诺克/诺玛双人格）
 * @version LN-B-5.0.0.0
 * @changes 双文支持（人格名称支持多语言）
 */

#include "../lib/platform.h"
#include "nook_personality.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include <string.h>

/* ============================================================
 * 人格定义（使用 tr() 宏支持多语言）
 * ============================================================ */

nook_personality_t nook_prof = {
    .name           = NULL,  /* 将在运行时通过 tr() 动态获取 */
    .gender         = GENDER_MALE,
    .voice_tone     = VOICE_MALE,
    .style_strict   = 3,
    .style_warmth   = 3,
    .style_speed    = 4
};

nook_personality_t noma_prof = {
    .name           = NULL,  /* 将在运行时通过 tr() 动态获取 */
    .gender         = GENDER_FEMALE,
    .voice_tone     = VOICE_FEMALE,
    .style_strict   = 2,
    .style_warmth   = 5,
    .style_speed    = 3
};

/* ============================================================
 * 获取人格名称（双文支持）
 * ============================================================ */

const char* nook_personality_get_name(nook_personality_t *prof) {
    if (!prof) return tr("Unknown", "未知");

    /* 根据人格特征返回对应的多语言名称 */
    if (prof->gender == GENDER_MALE && prof->voice_tone == VOICE_MALE) {
        return tr("Nook (Nook)", "诺克 (Nook)");
    } else if (prof->gender == GENDER_FEMALE && prof->voice_tone == VOICE_FEMALE) {
        return tr("Noma (Noma)", "诺玛 (Noma)");
    }

    return tr("Custom", "自定义");
}

/* ============================================================
 * 初始化人格名称（在加载时调用）
 * ============================================================ */

void nook_personality_init_names(void) {
    /* 动态设置人格名称（支持多语言） */
    nook_prof.name = nook_personality_get_name(&nook_prof);
    noma_prof.name = nook_personality_get_name(&noma_prof);
}

/* ============================================================
 * 获取当前语言下的人格描述
 * ============================================================ */

const char* nook_personality_get_description(nook_personality_t *prof) {
    if (!prof) return tr("Unknown personality", "未知人格");

    if (prof->gender == GENDER_MALE && prof->voice_tone == VOICE_MALE) {
        return tr(
            "Nook: Calm, rational, strictly loyal. The core AI of LING OS.",
            "诺克：冷静、理性、绝对忠诚。LING OS 的核心AI。"
        );
    } else if (prof->gender == GENDER_FEMALE && prof->voice_tone == VOICE_FEMALE) {
        return tr(
            "Noma: Gentle, warm, wise. The companion AI of LING OS.",
            "诺玛：温柔、温暖、睿智。LING OS 的陪伴AI。"
        );
    }

    return tr("Custom personality", "自定义人格");
}