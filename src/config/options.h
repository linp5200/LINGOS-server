/**
 * @file    src/config/options.h
 * @brief   可选项开关体系（先生 2026-09-12 定稿）
 * @version LN-0.5.0
 *
 * 设计原则：
 *   P1 核心必装，附加可选
 *   P2 安全默认最严（GDPR by default）
 *   P3 底线不可关（少数安全项无开关）
 *
 * 三档：
 *   OPT_KIND_FIXED    🔒 不可选（安全底线）
 *   OPT_KIND_ON       🟢 可选，默认开
 *   OPT_KIND_OFF      🔵 可选，默认关
 *
 * 「隐私保护模式」（原名「一键最严」）：
 *   一次性把所有安全增强项开到最严 + 相关隐私项开启。
 */

#ifndef LINGOS_OPTIONS_H
#define LINGOS_OPTIONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 分组
 * ============================================================ */
typedef enum {
    OPT_GROUP_SECURITY   = 0,   /* 安全增强 */
    OPT_GROUP_PRIVACY    = 1,   /* 隐私技术 */
    OPT_GROUP_FEATURE    = 2,   /* 功能增强 */
    OPT_GROUP_UI         = 3,   /* UI/显示 */
    OPT_GROUP_VOICE      = 4,   /* 语音 */
    OPT_GROUP_AI         = 5,   /* AI/模型 */
    OPT_GROUP_DATA       = 6,   /* 数据/同步 */
    OPT_GROUP_CONN       = 7,   /* 连接 */
    OPT_GROUP_DEV        = 8,   /* 【0.7.0 P2】开发调试（先生设定） */
    OPT_GROUP__COUNT
} option_group_t;

/* ============================================================
 * 可选性
 * ============================================================ */
typedef enum {
    OPT_KIND_FIXED = 0,   /* 🔒 不可选（底线） */
    OPT_KIND_ON,          /* 🟢 默认开 */
    OPT_KIND_OFF          /* 🔵 默认关 */
} option_kind_t;

/* ============================================================
 * 定义
 * ============================================================ */
typedef struct {
    const char    *key;        /* 配置键（唯一） */
    const char    *name_zh;
    const char    *name_en;
    option_group_t group;
    option_kind_t  kind;
    int            dangerous;      /* 危险开关（改动需二次确认 + UI 标识） */
    int            needs_condition;/* UI 标注「需条件」（HE 需云端 / FL 需多设备） */
    const char    *desc_zh;
    const char    *desc_en;
} option_def_t;

/* ============================================================
 * API
 * ============================================================ */

/** 初始化（加载持久化配置；不存在则用默认） */
int options_init(void);

/** 原子：取全部定义 */
const option_def_t *options_all(int *count);

/** 按 key 查找定义（NULL=未找到） */
const option_def_t *options_find(const char *key);

/** 读值：返回 0/1；-1 表示 key 不存在 */
int options_get(const char *key);

/** 写值并持久化；返回 0 成功，-1 失败（危险开关需 force=1） */
int options_set(const char *key, int value, int force);

/** 【隐私保护模式】一次性开启全部安全增强 + 隐私技术 */
int options_apply_privacy_mode(void);

/** 撤销隐私保护模式（恢复默认） */
int options_clear_privacy_mode(void);

/** 是否处于隐私保护模式 */
int options_in_privacy_mode(void);

/** 导出为 JSON（供 App/Web 渲染设置页） */
char *options_to_json(void);

#ifdef __cplusplus
}
#endif

#endif /* LINGOS_OPTIONS_H */
