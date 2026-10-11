#ifndef AI_AI_MASTER_H
#define AI_AI_MASTER_H

#include <stdint.h>
#include <stddef.h>
#include "../common/types.h"

/* ============================================================
 * 分类主 AI（Code / Guard / General）
 *   实装：真实分类（内容/意图打分）+ 真实审查（危险规则表 + ai_privilege 权限结果）
 * ============================================================ */

typedef enum {
    MASTER_CODE,
    MASTER_GUARD,
    MASTER_GENERAL
} master_ai_type_t;

typedef struct {
    uint8_t approved;         /* 1 批准，0 不批准（默认为 0：fail-closed） */
    uint8_t needs_revision;   /* 1 需要修订 */
    char    suggestion[256];  /* 真实结论说明（命中规则/权限状态） */
} master_review_t;

/* 审查来源：不可信来源会触发更严格规则 */
typedef enum {
    AI_MASTER_SOURCE_LOCAL = 0,  /* 本机（TUI / 守护进程内部） */
    AI_MASTER_SOURCE_MQTT  = 1,  /* MQTT（网络输入，不可信） */
    AI_MASTER_SOURCE_API   = 2   /* HTTP / WS API */
} ai_master_source_t;

/* 审查请求（真实输入） */
typedef struct {
    const char *content;     /* 待审查内容（命令参数/回答/代码）；NULL 或空 → 拒绝 */
    const char *skill_name;  /* 关联技能名：走 ai_privilege 规则/权限结果，可为 NULL */
    int         source;      /* ai_master_source_t */
} ai_master_request_t;

/**
 * @brief 初始化分类主 AI
 * @note  真实接线：加载 ai_privilege 规则表 + 注册 MQTT 命令审查钩子
 */
void ai_master_init(void);

/**
 * @brief 真实分类：按内容/意图关键词打分
 * @param content 待分类内容
 * @return master_ai_type_t（无法判定时返回 MASTER_GENERAL）
 */
int ai_master_classify(const char *content);

/**
 * @brief 真实审查（带上下文）
 * @param master_type 分类结果
 * @param request 审查请求（content/skill_name/source）
 * @param review 输出审查结论
 * @return 0 已审查（结论在 review 中），-1 参数错误
 */
int ai_master_review_ex(master_ai_type_t master_type,
                        const ai_master_request_t *request,
                        master_review_t *review);

/**
 * @brief 兼容接口：sub_result 视为 NUL 结尾的待审内容字符串
 * @return 0 已审查，-1 参数错误
 */
int ai_master_review(master_ai_type_t master_type,
                     void *sub_result,
                     master_review_t *review);

/**
 * @brief 获取主AI类型名称（根据当前语言）
 */
const char *ai_master_type_name(master_ai_type_t type);

/**
 * @brief MQTT 命令审查适配器（注册到 mqtt_ha 审查钩子）
 * @return 0 放行，非 0 拒绝（reason 回填原因）
 */
int ai_master_ha_review(const char *command, const char *args_json,
                        char *reason, size_t reason_len);

#endif
