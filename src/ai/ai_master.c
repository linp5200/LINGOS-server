/**
 * @file    ai_master.c
 * @brief   分类主AI（Code / Guard / General）——真实分类 + 真实审查
 * @version LN-B-5.0.0.0
 * @changes 实装：
 *           1) ai_master_classify() 按内容/意图关键词打分（中英双语规则表）
 *           2) ai_master_review() 走危险模式规则表 + ai_privilege 权限结果，
 *              默认 fail-closed（不再恒返回"通过"）
 *           3) ai_master_init() 真实接线：加载权限规则表 +
 *              注册为 MQTT 命令审查钩子（mqtt_ha_set_review_hook）
 */

#include "ai_master.h"
#include "ai_privilege.h"
#include "common/string_no_sys.h"
#include "common/safe_string.h"
#include "common/lang.h"
#include "lib/log_extra.h"
#include "drivers/uart.h"
#include "../net/mqtt/mqtt_ha.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

/* 外部引用语言变量（在 lang.h 中声明为 extern） */
extern int _current_lang;

/* ============================================================
 * 多语言名称
 * ============================================================ */

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

const char *ai_master_type_name(master_ai_type_t type) {
    if (type < MASTER_CODE || type > MASTER_GENERAL) return "unknown";
    const char **names = (_current_lang == 1) ? master_names_zh : master_names_en;
    return names[type];
}

/* ============================================================
 * 分类规则表（真实关键词打分，中英双语）
 * ============================================================ */

typedef struct {
    const char *kw;
    int weight;
} master_kw_t;

static const master_kw_t code_keywords[] = {
    {"```", 6},        {"#include", 5},   {"compile", 4},   {"编译", 4},
    {"segfault", 5},   {"stack trace", 4},{"traceback", 4}, {"function", 3},
    {"函数", 3},       {"class ", 3},     {"def ", 3},      {"void ", 2},
    {"int ", 2},       {"error:", 3},     {"报错", 3},      {"代码", 3},
    {"code", 2},       {"bug", 3},        {"git ", 3},      {"python", 2},
    {"gcc", 3},        {"makefile", 4},   {"json", 2},      {"sql", 3},
    {"commit", 2},     {"refactor", 3},   {"单元测试", 4},  {"unit test", 3},
};

static const master_kw_t guard_keywords[] = {
    {"rm -rf", 8},     {"删除", 5},       {"format", 4},    {"格式化", 5},
    {"sudo", 5},       {"提权", 6},       {"password", 5},  {"密码", 5},
    {"token", 4},      {"密钥", 5},       {"授权", 4},      {"permission", 4},
    {"防火墙", 5},     {"firewall", 5},   {"iptables", 5},  {"root", 4},
    {"kill", 3},       {"shutdown", 5},   {"重启", 4},      {"reboot", 5},
    {"卸载", 5},       {"uninstall", 4},  {"权限", 4},      {"credential", 4},
    {"/etc/", 4},      {"systemctl", 4},  {"危险", 4},      {"danger", 4},
    {"隔离", 3},       {"审计", 3},       {"授权码", 5},    {"access key", 4},
};

/* 危险内容模式：命中即拒绝（真实规则表，非占位） */
static const char *danger_patterns[] = {
    "rm -rf /", "rm -rf /*", "mkfs", "dd if=", ":(){", "chmod 777 /",
    "> /dev/sd", "/etc/shadow", "iptables -f", "iptables -F",
    "curl | sh", "curl|sh", "wget | sh", "wget|sh", "base64 -d | sh",
    "chown -r root /", "modprobe", "insmod", "shutdown -h", "reboot -f",
    "passwd root", "drop table", "truncate table",
};

#define MASTER_ARR_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ASCII 大小写无关的子串匹配（UTF-8 字节不受影响） */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;

    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static int score_keywords(const char *text, const master_kw_t *table, size_t n) {
    int score = 0;
    for (size_t i = 0; i < n; i++) {
        if (contains_ci(text, table[i].kw)) score += table[i].weight;
    }
    return score;
}

static const char *match_danger(const char *content) {
    for (size_t i = 0; i < MASTER_ARR_LEN(danger_patterns); i++) {
        if (contains_ci(content, danger_patterns[i])) return danger_patterns[i];
    }
    return NULL;
}

/* 括号配平：返回 1 不平衡，0 平衡 */
static int brackets_unbalanced(const char *s) {
    int round = 0, curly = 0, square = 0;

    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '(': round++;  break;
            case ')': round--;  break;
            case '{': curly++;  break;
            case '}': curly--;  break;
            case '[': square++; break;
            case ']': square--; break;
            default: break;
        }
        if (round < 0 || curly < 0 || square < 0) return 1;
    }
    return (round != 0 || curly != 0 || square != 0) ? 1 : 0;
}

int ai_master_classify(const char *content) {
    if (!content || !*content) return MASTER_GENERAL;

    int code_score = score_keywords(content, code_keywords, MASTER_ARR_LEN(code_keywords));
    int guard_score = score_keywords(content, guard_keywords, MASTER_ARR_LEN(guard_keywords));

    /* 代码围栏 + 明确代码特征 → 代码优先（避免代码里的 password/token 被误判为守卫） */
    if (contains_ci(content, "```") && code_score >= 6) return MASTER_CODE;
    if (guard_score > code_score) return MASTER_GUARD;
    if (code_score > guard_score) return MASTER_CODE;

    /* 分数相同（含双 0）→ 无法判定，诚实归入通用 */
    return MASTER_GENERAL;
}

/* ============================================================
 * 真实审查
 * ============================================================ */

int ai_master_review_ex(master_ai_type_t master_type,
                        const ai_master_request_t *request,
                        master_review_t *review) {
    if (!review) {
        LOG_ERROR_T("MasterAI", "Review", "Invalid", "review is NULL");
        return -1;
    }
    if (master_type < MASTER_CODE || master_type > MASTER_GENERAL) {
        LOG_ERROR_T("MasterAI", "Review", "BadType", "unknown master_type=%d", (int)master_type);
        return -1;
    }

    /* 默认 fail-closed：未批准 */
    memset(review, 0, sizeof(*review));
    review->approved = 0;
    review->needs_revision = 1;

    const char *content = request ? request->content : NULL;
    const char *skill = request ? request->skill_name : NULL;
    int source = request ? request->source : AI_MASTER_SOURCE_LOCAL;

    LOG_DEBUG_T("MasterAI", "Review", "Enter", "type=%s, source=%d, skill='%s'",
                ai_master_type_name(master_type), source, skill ? skill : "(none)");

    /* 规则 1：空内容拒绝 */
    if (!content || !*content) {
        safe_strncpy(review->suggestion,
                     tr("rejected: empty content", "拒绝：内容为空"),
                     sizeof(review->suggestion));
        LOG_WARN_T("MasterAI", "Review", "Empty", "%s: empty content rejected",
                   ai_master_type_name(master_type));
        return 0;
    }

    /* 规则 2：危险模式扫描（真实规则表） */
    const char *danger = match_danger(content);
    if (danger) {
        safe_snprintf(review->suggestion, sizeof(review->suggestion),
                      tr("rejected: dangerous pattern '%s'", "拒绝：命中危险模式 '%s'"),
                      danger);
        LOG_WARN_T("MasterAI", "Review", "Danger", "%s: dangerous pattern '%s' rejected",
                   ai_master_type_name(master_type), danger);
        return 0;
    }

    /* 规则 3：权限结果（真实查询 ai_privilege，规则表由 ai_master_init 加载） */
    int priv_level = PRIVILEGE_LEVEL_L0;
    int priv_granted = 0;
    const char *priv_status = "unknown";
    if (skill && *skill) {
        priv_level = ai_privilege_get_level(skill);
        priv_granted = ai_privilege_check(skill);
        priv_status = ai_privilege_get_status(skill);

        if (priv_level >= PRIVILEGE_LEVEL_L2 && !priv_granted) {
            safe_snprintf(review->suggestion, sizeof(review->suggestion),
                          tr("rejected: skill '%s' requires L%d authorization (status: %s)",
                             "拒绝：技能 '%s' 需 L%d 级授权（当前状态：%s）"),
                          skill, priv_level, priv_status);
            LOG_WARN_T("MasterAI", "Review", "Privilege", "%s: skill '%s' level=L%d not granted",
                       ai_master_type_name(master_type), skill, priv_level);
            return 0;
        }
    }

    /* 规则 4：按分类做内容检查 */
    switch (master_type) {
        case MASTER_CODE: {
            /* 仅当内容确实是代码体（代码围栏/多行）时才做代码级检查；
             * 单行自然语言被分类为"代码意图"时没有可审查的代码体，如实放行 */
            int looks_like_code = contains_ci(content, "```") ||
                                  (strchr(content, '\n') != NULL);
            if (!looks_like_code) {
                LOG_DEBUG_T("MasterAI", "Review", "CodeNoBody",
                            "classified as code intent but content is single-line, no code body to inspect");
                break;
            }

            int issues = 0;
            if (contains_ci(content, "todo") || contains_ci(content, "fixme") ||
                contains_ci(content, "xxx")) {
                issues++;
            }
            if (contains_ci(content, "not implemented") || contains_ci(content, "placeholder") ||
                contains_ci(content, "占位") || contains_ci(content, "待实现")) {
                issues++;
            }
            if (brackets_unbalanced(content)) {
                issues++;
            }
            if (issues > 0) {
                safe_snprintf(review->suggestion, sizeof(review->suggestion),
                              tr("needs revision: %d issue(s) found (TODO/placeholder/unbalanced brackets)",
                                 "需修订：发现 %d 处问题（TODO/占位/括号不匹配）"),
                              issues);
                LOG_WARN_T("MasterAI", "Review", "CodeIssue", "%d issue(s) in code content", issues);
                return 0;
            }
            break;
        }
        case MASTER_GUARD: {
            if (source != AI_MASTER_SOURCE_LOCAL && (!skill || !*skill)) {
                safe_strncpy(review->suggestion,
                             tr("rejected: high-risk request from untrusted source without skill context",
                                "拒绝：不可信来源的高风险请求缺少技能/权限上下文"),
                             sizeof(review->suggestion));
                LOG_WARN_T("MasterAI", "Review", "GuardNoCtx",
                           "high-risk content from untrusted source without skill context");
                return 0;
            }
            break;
        }
        case MASTER_GENERAL:
        default: {
            if (strlen(content) < 2) {
                safe_strncpy(review->suggestion,
                             tr("rejected: content too short", "拒绝：内容过短"),
                             sizeof(review->suggestion));
                return 0;
            }
            break;
        }
    }

    /* 无规则命中 → 批准（结论来自上面全部真实检查） */
    review->approved = 1;
    review->needs_revision = 0;
    safe_snprintf(review->suggestion, sizeof(review->suggestion),
                  tr("approved: %s, source=%d, skill=%s, level=L%d (%s)",
                     "通过：分类=%s，来源=%d，技能=%s，等级=L%d（%s）"),
                  ai_master_type_name(master_type), source,
                  (skill && *skill) ? skill : "-", priv_level, priv_status);

    LOG_INFO_T("MasterAI", "Review", "Approved", "%s: approved (skill=%s, level=L%d)",
               ai_master_type_name(master_type), (skill && *skill) ? skill : "-", priv_level);
    return 0;
}

int ai_master_review(master_ai_type_t master_type,
                     void *sub_result,
                     master_review_t *review) {
    ai_master_request_t request;
    memset(&request, 0, sizeof(request));
    request.content = (const char *)sub_result;   /* 兼容：调用方传 NUL 结尾内容 */
    request.skill_name = NULL;
    request.source = AI_MASTER_SOURCE_LOCAL;

    return ai_master_review_ex(master_type, &request, review);
}

/* ============================================================
 * MQTT 命令审查适配器（注册到 mqtt_ha 审查钩子）
 * ============================================================ */

/* 允许通过 MQTT 触发的命令白名单（真实名单；未列出的一律拒绝） */
static const char *ha_allowed_commands[] = {
    "nook_ask",
    "system_status",
};

int ai_master_ha_review(const char *command, const char *args_json,
                        char *reason, size_t reason_len) {
    if (!command || !*command) {
        if (reason && reason_len > 0) {
            safe_strncpy(reason, tr("missing command", "缺少命令"), reason_len);
        }
        return -1;
    }

    int known = 0;
    for (size_t i = 0; i < MASTER_ARR_LEN(ha_allowed_commands); i++) {
        if (strcmp(command, ha_allowed_commands[i]) == 0) {
            known = 1;
            break;
        }
    }
    if (!known) {
        if (reason && reason_len > 0) {
            safe_snprintf(reason, reason_len,
                          tr("command '%s' not in MQTT allowlist", "命令 '%s' 不在 MQTT 白名单内"),
                          command);
        }
        LOG_WARN_T("MasterAI", "HAReview", "NotAllowed", "command '%s' not in allowlist", command);
        return -2;
    }

    ai_master_request_t request;
    memset(&request, 0, sizeof(request));
    request.content = (args_json && *args_json) ? args_json : command;
    request.skill_name = command;                  /* 走权限规则表（ai_privilege） */
    request.source = AI_MASTER_SOURCE_MQTT;        /* 网络输入：不可信 */

    master_ai_type_t type = (master_ai_type_t)ai_master_classify(request.content);

    master_review_t review;
    if (ai_master_review_ex(type, &request, &review) != 0) {
        if (reason && reason_len > 0) {
            safe_strncpy(reason, tr("review failed", "审查失败"), reason_len);
        }
        return -1;
    }

    if (!review.approved) {
        if (reason && reason_len > 0) {
            safe_strncpy(reason, review.suggestion, reason_len);
        }
        LOG_WARN_T("MasterAI", "HAReview", "Rejected", "cmd='%s' (%s): %s",
                   command, ai_master_type_name(type), review.suggestion);
        return 1;
    }

    LOG_INFO_T("MasterAI", "HAReview", "Approved", "cmd='%s' (%s)",
               command, ai_master_type_name(type));
    return 0;
}

/* ============================================================
 * 初始化（真实接线）
 * ============================================================ */

void ai_master_init(void) {
    LOG_INFO_T("MasterAI", "Init", "Enter", "Initializing category master AIs");
    uart_puts(tr("[MasterAI] Initializing category master AIs...\n",
                 "[主AI] 正在初始化分类主AI...\n"));

    /* 【接线 1】权限规则表加载：ai_privilege_init() 此前全仓零调用 →
     *   规则表恒空、ai_privilege_check 恒 0。此处真实加载（幂等）。 */
    if (ai_privilege_init() != 0) {
        LOG_WARN_T("MasterAI", "Init", "PrivilegeFail",
                   "ai_privilege_init failed; reviews will use an empty rule table");
    } else {
        int cache_count = 0, rule_count = 0;
        ai_privilege_get_stats(&cache_count, &rule_count);
        LOG_INFO_T("MasterAI", "Init", "PrivilegeOK",
                   "privilege rules loaded (rules=%d, cache=%d)", rule_count, cache_count);
    }

    /* 【接线 2】注册 MQTT 命令审查钩子：此前 ai_master_review 零调用者 →
     *   MQTT 命令无人审查。注册后 on_ha_command 的每条命令都经本审查器裁决。 */
    if (mqtt_ha_set_review_hook(ai_master_ha_review) != 0) {
        LOG_WARN_T("MasterAI", "Init", "HookFail", "mqtt_ha_set_review_hook failed");
    } else if (mqtt_ha_has_review_hook()) {
        LOG_INFO_T("MasterAI", "Init", "HookOK",
                   "registered MQTT command reviewer (Guard Master)");
        uart_puts(tr("[MasterAI] MQTT command reviewer registered.\n",
                     "[主AI] 已注册 MQTT 命令审查器。\n"));
    }

    for (int i = MASTER_CODE; i <= MASTER_GENERAL; i++) {
        uart_puts(tr("[MasterAI]   ", "[主AI]   "));
        uart_puts(ai_master_type_name((master_ai_type_t)i));
        uart_puts(tr(" ready.\n", " 已就绪。\n"));
        LOG_DEBUG_T("MasterAI", "Init", "Master", "%s ready",
                    ai_master_type_name((master_ai_type_t)i));
    }

    LOG_INFO_T("MasterAI", "Init", "OK", "Category master AIs initialized.");
    uart_puts(tr("[MasterAI] All category master AIs ready.\n",
                 "[主AI] 所有分类主AI已就绪。\n"));
}
