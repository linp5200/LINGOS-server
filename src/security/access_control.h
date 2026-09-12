/**
 * @file    src/security/access_control.h
 * @brief   访问控制（先生 2026-09-12 裁决 S9/S11/S18 落地）
 * @version LN-0.5.0
 *
 * 先生裁决：
 *   S9  —— 局域网内免 token（高风险命令需二次确认，除非开
 *          《允许局域网使用 API 运行高风险命令》）；公网需 token
 *   S11 —— CORS 去掉 `*`，改为校验 Origin（同源/白名单）
 *   S18 —— 默认限流；可开《允许局域网内连接不限流》
 *
 * 威胁模型（先生裁决）：C —— 可能暴露公网
 */

#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 网络来源分级
 * ============================================================ */
typedef enum {
    NET_SCOPE_LOCALHOST = 0,   /* 127.0.0.0/8 ::1          —— 完全信任 */
    NET_SCOPE_LAN,             /* RFC1918 / 链路本地        —— 局域网（免 token） */
    NET_SCOPE_PUBLIC           /* 其他                       —— 公网（需 token） */
} net_scope_t;

/* ============================================================
 * 配置（可由 config 系统读写）
 * ============================================================ */
typedef struct {
    int lan_no_token;             /* 局域网免 token（默认 1） */
    int allow_lan_highrisk;       /* 《允许局域网使用 API 运行高风险命令》（默认 0） */
    int allow_lan_no_ratelimit;   /* 《允许局域网内连接不限流》（默认 0） */
    int rate_limit_per_min;       /* 每 IP 每分钟上限（默认 120） */
    int auth_code_per_min;        /* 认证码尝试上限/分钟（默认 5） */
    int cors_strict;              /* CORS 校验 Origin（默认 1） */
} access_config_t;

/* ============================================================
 * 初始化与配置
 * ============================================================ */
void access_control_init(void);
access_config_t *access_control_config(void);

int  access_config_get_bool(const char *key);      /* 供 config 系统读取 */
int  access_config_set_bool(const char *key, int v);
int  access_config_set_int(const char *key, int v);

/* ============================================================
 * 来源判定
 * ============================================================ */
net_scope_t access_classify_ip(const char *ip);

/**
 * @brief 语义化：是否需要 token
 *        localhost → 否；LAN 且 lan_no_token → 否；其余 → 是
 */
int access_needs_token(const char *ip, int has_token);

/**
 * @brief 高风险命令是否需要二次确认
 *        localhost → 否；LAN 且 allow_lan_highrisk → 否；其余 → 是
 */
int access_highrisk_needs_confirm(const char *ip);

/* ============================================================
 * 限流（令牌桶/滑动窗口——简化实现）
 * ============================================================ */
/** @return 1 允许；0 限流拒绝 */
int access_rate_allow(const char *ip);
/** 认证码专用限流（防爆破，先生 S18） */
int access_authcode_allow(const char *ip);

/* ============================================================
 * CORS（先生 S11：去掉 `*`）
 * ============================================================ */
/**
 * @brief 判定 Origin 是否允许（同源 / 白名单 / 无 Origin）
 * @return 1 允许；0 拒绝
 */
int access_cors_allow(const char *origin, const char *host_header);

/**
 * @brief 写 CORS 响应头（仅在允许时写；不允许则不写 —— 浏览器自会阻止）
 */
void access_cors_add_headers(void *resp, const char *origin, const char *host_header);

#ifdef __cplusplus
}
#endif

#endif /* ACCESS_CONTROL_H */
