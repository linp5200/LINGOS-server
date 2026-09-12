/**
 * @file    src/security/access_control.c
 * @brief   访问控制实现（S9/S11/S18）
 * @version LN-0.5.0
 *
 * 设计（对照国际标准）：
 *   · OWASP IoT I2「不安全的网络服务」—— 认证按暴露面分级
 *   · OWASP IoT I9「不安全的默认设置」—— 默认最严（GDPR by default）
 *   · OWASP LLM10「无限消耗」—— 限流/超时/资源管理
 */

#include "access_control.h"
#include "log_extra.h"
#include "safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

/* ============================================================
 * 配置默认值（先生裁决：默认最严）
 * ============================================================ */
static access_config_t g_cfg = {
    .lan_no_token           = 1,    /* 先生 S9：局域网免 token */
    .allow_lan_highrisk     = 0,    /* 默认不许（需显式开启） */
    .allow_lan_no_ratelimit = 0,    /* 默认限流（需显式关闭） */
    .rate_limit_per_min     = 120,  /* 每 IP 每分钟 120 次 */
    .auth_code_per_min      = 5,    /* 认证码每分钟 5 次（防爆破） */
    .cors_strict            = 1,    /* 默认严格 CORS */
};

void access_control_init(void) {
    const char *v;
    v = getenv("LINGOS_LAN_NO_TOKEN");
    if (v) g_cfg.lan_no_token = (atoi(v) != 0);
    v = getenv("LINGOS_ALLOW_LAN_HIGHRISK");
    if (v) g_cfg.allow_lan_highrisk = (atoi(v) != 0);
    v = getenv("LINGOS_ALLOW_LAN_NO_RATELIMIT");
    if (v) g_cfg.allow_lan_no_ratelimit = (atoi(v) != 0);
    LOG_INFO_T("AccessCtl", "Init", "OK",
               "lan_no_token=%d allow_highrisk=%d allow_norate=%d rate=%d/min",
               g_cfg.lan_no_token, g_cfg.allow_lan_highrisk,
               g_cfg.allow_lan_no_ratelimit, g_cfg.rate_limit_per_min);
}

access_config_t *access_control_config(void) { return &g_cfg; }

int access_config_get_bool(const char *key) {
    if (!key) return -1;
    if (!strcmp(key, "lan_no_token"))           return g_cfg.lan_no_token;
    if (!strcmp(key, "allow_lan_highrisk"))     return g_cfg.allow_lan_highrisk;
    if (!strcmp(key, "allow_lan_no_ratelimit")) return g_cfg.allow_lan_no_ratelimit;
    if (!strcmp(key, "cors_strict"))            return g_cfg.cors_strict;
    return -1;
}

int access_config_set_bool(const char *key, int v) {
    if (!key) return -1;
    if (!strcmp(key, "lan_no_token"))           { g_cfg.lan_no_token = !!v; return 0; }
    if (!strcmp(key, "allow_lan_highrisk"))     { g_cfg.allow_lan_highrisk = !!v; return 0; }
    if (!strcmp(key, "allow_lan_no_ratelimit")) { g_cfg.allow_lan_no_ratelimit = !!v; return 0; }
    if (!strcmp(key, "cors_strict"))            { g_cfg.cors_strict = !!v; return 0; }
    return -1;
}

int access_config_set_int(const char *key, int v) {
    if (!key) return -1;
    if (!strcmp(key, "rate_limit_per_min")) { g_cfg.rate_limit_per_min = (v > 0 ? v : 1); return 0; }
    if (!strcmp(key, "auth_code_per_min"))  { g_cfg.auth_code_per_min  = (v > 0 ? v : 1); return 1 - 1; }
    return -1;
}

/* ============================================================
 * IP 分类
 * ============================================================ */
static int parse_ipv4(const char *ip, unsigned int *out) {
    unsigned int a, b, c, d;
    if (!ip) return 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

net_scope_t access_classify_ip(const char *ip) {
    if (!ip || !*ip) return NET_SCOPE_PUBLIC;   /* 未知 → 当公网（最严）*/

    /* IPv6 */
    if (strchr(ip, ':')) {
        if (!strcmp(ip, "::1") || !strcmp(ip, "::ffff:127.0.0.1"))
            return NET_SCOPE_LOCALHOST;
        /* fc00::/7 唯一本地；fe80::/10 链路本地 */
        if (!strncasecmp(ip, "fc", 2) || !strncasecmp(ip, "fd", 2) ||
            !strncasecmp(ip, "fe8", 3) || !strncasecmp(ip, "fe9", 3) ||
            !strncasecmp(ip, "fea", 3) || !strncasecmp(ip, "feb", 3))
            return NET_SCOPE_LAN;
        return NET_SCOPE_PUBLIC;
    }

    unsigned int v;
    if (!parse_ipv4(ip, &v)) return NET_SCOPE_PUBLIC;

    unsigned int a = (v >> 24) & 0xFF, b = (v >> 16) & 0xFF;
    /* 回环 127.0.0.0/8 */
    if (a == 127) return NET_SCOPE_LOCALHOST;
    /* 10.0.0.0/8 */
    if (a == 10) return NET_SCOPE_LAN;
    /* 172.16.0.0/12 —— ★ 精确：16..31（原实现误伤 172.32+） */
    if (a == 172 && b >= 16 && b <= 31) return NET_SCOPE_LAN;
    /* 192.168.0.0/16 */
    if (a == 192 && b == 168) return NET_SCOPE_LAN;
    /* 169.254.0.0/16 链路本地 —— 视为 LAN 但访问元数据由 SSRF 层拦 */
    if (a == 169 && b == 254) return NET_SCOPE_LAN;
    /* 0.0.0.0/8 */
    if (a == 0) return NET_SCOPE_LOCALHOST;

    return NET_SCOPE_PUBLIC;
}

/* ============================================================
 * 认证决策
 * ============================================================ */
int access_needs_token(const char *ip, int has_token) {
    net_scope_t s = access_classify_ip(ip);
    if (s == NET_SCOPE_LOCALHOST) return 0;          /* 本机 → 免 */
    if (s == NET_SCOPE_LAN && g_cfg.lan_no_token) {
        (void)has_token;
        return 0;                                     /* 先生 S9：局域网免 token */
    }
    return 1;                                          /* 公网 → 必须 */
}

int access_highrisk_needs_confirm(const char *ip) {
    net_scope_t s = access_classify_ip(ip);
    if (s == NET_SCOPE_LOCALHOST) return 0;
    if (s == NET_SCOPE_LAN && g_cfg.allow_lan_highrisk) return 0;
    return 1;   /* 默认需二次确认（先生 S9） */
}

/* ============================================================
 * 限流（滑动窗口，按 IP）
 * ============================================================ */
#define RL_SLOTS 64
typedef struct {
    char     ip[64];
    int      count;
    int      burst;              /* 认证码等高频尝试计数 */
    time_t   window_start;
    time_t   burst_start;
    int      used;
} rl_slot_t;

static rl_slot_t g_slots[RL_SLOTS];
static pthread_mutex_t g_rl_lock = PTHREAD_MUTEX_INITIALIZER;

static rl_slot_t *rl_get(const char *ip) {
    time_t now = time(NULL);
    int free_idx = -1;
    for (int i = 0; i < RL_SLOTS; i++) {
        if (g_slots[i].used && strcmp(g_slots[i].ip, ip) == 0) return &g_slots[i];
        if (!g_slots[i].used && free_idx < 0) free_idx = i;
    }
    /* 回收过期槽 */
    if (free_idx < 0) {
        for (int i = 0; i < RL_SLOTS; i++) {
            if (now - g_slots[i].window_start > 120) { free_idx = i; break; }
        }
    }
    if (free_idx < 0) free_idx = 0;
    memset(&g_slots[free_idx], 0, sizeof(rl_slot_t));
    safe_strncpy(g_slots[free_idx].ip, ip, sizeof(g_slots[free_idx].ip));
    g_slots[free_idx].used = 1;
    g_slots[free_idx].window_start = now;
    g_slots[free_idx].burst_start = now;
    return &g_slots[free_idx];
}

int access_rate_allow(const char *ip) {
    net_scope_t s = access_classify_ip(ip);
    /* 本机不限流（否则本地工具会被卡） */
    if (s == NET_SCOPE_LOCALHOST) return 1;
    /* 先生 S18：局域网内可配置不限流 */
    if (s == NET_SCOPE_LAN && g_cfg.allow_lan_no_ratelimit) return 1;

    pthread_mutex_lock(&g_rl_lock);
    rl_slot_t *sl = rl_get(ip);
    time_t now = time(NULL);
    if (now - sl->window_start >= 60) {
        sl->window_start = now;
        sl->count = 0;
    }
    int allow = (sl->count < g_cfg.rate_limit_per_min);
    if (allow) sl->count++;
    pthread_mutex_unlock(&g_rl_lock);

    if (!allow) {
        LOG_WARN_T("AccessCtl", "RateLimit", "Blocked", "ip=%s count=%d/%d",
                   ip, sl->count, g_cfg.rate_limit_per_min);
    }
    return allow;
}

int access_authcode_allow(const char *ip) {
    pthread_mutex_lock(&g_rl_lock);
    rl_slot_t *sl = rl_get(ip);
    time_t now = time(NULL);
    if (now - sl->burst_start >= 60) {
        sl->burst_start = now;
        sl->burst = 0;
    }
    int allow = (sl->burst < g_cfg.auth_code_per_min);
    if (allow) sl->burst++;
    pthread_mutex_unlock(&g_rl_lock);
    if (!allow)
        LOG_WARN_T("AccessCtl", "AuthCodeLimit", "Blocked", "ip=%s burst=%d/%d",
                   ip, sl->burst, g_cfg.auth_code_per_min);
    return allow;
}

/* ============================================================
 * CORS（先生 S11：去掉 `*`）
 * ============================================================ */
/* 同源判定：Origin 的 host:port 与请求 Host 一致 → 允许 */
static int same_origin(const char *origin, const char *host) {
    if (!origin || !host) return 0;
    const char *p = strstr(origin, "://");
    if (!p) return 0;
    p += 3;
    /* 去掉尾部斜杠 */
    size_t ol = strlen(p);
    while (ol > 0 && p[ol - 1] == '/') ol--;
    size_t hl = strlen(host);
    if (ol != hl) return 0;
    return strncasecmp(p, host, hl) == 0;
}

int access_cors_allow(const char *origin, const char *host_header) {
    /* 无 Origin（同源请求/非浏览器客户端）→ 允许（不涉及 CORS） */
    if (!origin || !*origin) return 1;

    if (!g_cfg.cors_strict) {
        LOG_WARN_T("AccessCtl", "CORS", "Loose", "cors_strict=0 → 允许任意 Origin（不推荐）");
        return 1;
    }
    /* 同源 */
    if (same_origin(origin, host_header)) return 1;
    /* 本机来源 */
    if (strstr(origin, "://127.0.0.1") || strstr(origin, "://localhost") ||
        strstr(origin, "://[::1]"))
        return 1;

    LOG_WARN_T("AccessCtl", "CORS", "Rejected", "origin=%s host=%s",
               origin, host_header ? host_header : "(none)");
    return 0;
}

/*
 * 【0.5.0】CORS 头发射通过**弱符号**绑定：
 *   提供者 = http_server.c（含 microhttpd 的二进制）
 *   未提供者 = lingos_supervisor 等（不处理 HTTP）→ 弱符号为 NULL，安全跳过
 * 这样 access_control 无需依赖 microhttpd，也不会导致 supervisor 链接失败。
 */
extern int access_cors_emit_header(void *resp, const char *name, const char *value) __attribute__((weak));

void access_cors_add_headers(void *resp, const char *origin, const char *host_header) {
    if (!access_cors_emit_header) return;                 /* 无 HTTP 层 → 跳过 */
    if (!access_cors_allow(origin, host_header)) return;   /* 不允许 → 不写头 */
    if (origin && *origin) {
        char v[512];
        safe_snprintf(v, sizeof(v), "%s", origin);
        access_cors_emit_header(resp, "Access-Control-Allow-Origin", v);
        access_cors_emit_header(resp, "Vary", "Origin");
        access_cors_emit_header(resp, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        access_cors_emit_header(resp, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    }
}
