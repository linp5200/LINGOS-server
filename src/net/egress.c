/**
 * @file    src/net/egress.c
 * @brief   出口白名单实现（S2——防 AI 被诱导外连；先生裁决"默认最严"）
 * @version LN-0.7.0
 *
 * 实现说明：
 *   · 首次检查时加载清单（内置 + 用户文件合并），此后进程内缓存
 *   · 用户文件 enabled=false → 全部放行（与选项等效的显式覆盖）
 *   · options_get 弱符号：无 options 模块的二进制（alertd 等）按"开启"处理
 *   · 清单加载失败/文件缺失 → 仅用内置（不影响生命线）
 */

#include "egress.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* options_get 弱符号（alertd 等不含 options 模块的二进制 → NULL → 默认开启） */
extern int options_get(const char *key) __attribute__((weak));

#define EGRESS_MAX_ITEMS 256
#define EGRESS_MAX_ITEM_LEN 128

/* ============================================================
 * 内置默认清单（宁全勿缺——生命线/LLM/更新/搜索/语音全覆盖）
 * ============================================================ */
static const char *g_default_allow[] = {
    /* 生命线数据源 */
    "api.wolfx.jp",              /* EEW 四源（cenc/sc/cwa/jma） */
    "earthquake.usgs.gov",       /* USGS 地震 */
    "typhoon.nmc.cn",            /* 台风 NMC */
    "www.nmc.cn",
    /* 天气 */
    "api.open-meteo.com", "geocoding-api.open-meteo.com", "wttr.in",
    /* AI / LLM 服务（常用 provider 域集合——用户自有 provider 可加清单文件） */
    "api.deepseek.com", "dashscope.aliyuncs.com", "ark.cn-beijing.volces.com",
    "api.minimax.chat", "api.openai.com", "api.anthropic.com", "openrouter.ai",
    "api.moonshot.cn", "api.siliconflow.cn", "generativelanguage.googleapis.com",
    "api.x.ai", "api.z.ai", "api.mistral.ai", "api.groq.com", "api.together.xyz",
    "api.baichuan-ai.com", "api.lingyiwanwu.com", "api.360.cn", "api.hunyuan.cloud.tencent.com",
    /* 语音 */
    "api.deepgram.com", "api.elevenlabs.io", "eastasia.tts.speech.microsoft.com",
    "alphacephei.com",
    /* 搜索 */
    "html.duckduckgo.com", "duckduckgo.com", "www.google.com",
    "www.bing.com", "cn.bing.com",
    /* 更新 / 包源 */
    "github.com", "api.github.com", "raw.githubusercontent.com",
    "objects.githubusercontent.com", "codeload.github.com",
    "archive.ubuntu.com", "pypi.tuna.tsinghua.edu.cn",
    /* 通知 */
    "ntfy.sh",
    /* 其他集成 */
    "api.xiaomi.com",
    NULL
};

/* ============================================================
 * 清单缓存
 * ============================================================ */
static char  g_allow[EGRESS_MAX_ITEMS][EGRESS_MAX_ITEM_LEN];
static int   g_allow_count = 0;
static int   g_loaded = 0;
static int   g_user_disabled = 0;

static void egress_add(const char *item) {
    if (!item || !*item) return;
    if (g_allow_count >= EGRESS_MAX_ITEMS) return;
    /* 去重 */
    for (int i = 0; i < g_allow_count; i++) {
        if (strcasecmp(g_allow[i], item) == 0) return;
    }
    safe_strncpy(g_allow[g_allow_count], item, EGRESS_MAX_ITEM_LEN);
    /* 统一小写 */
    for (char *p = g_allow[g_allow_count]; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    g_allow_count++;
}

/* 极简 JSON 读取：从 {"domains":["a","b"]} 提取字符串数组（避免依赖 cJSON 于 alertd 场景） */
static void egress_load_user_file(void) {
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/egress_allowlist.json", lingos_data_root());
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char buf[8192];
    size_t r = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[r] = '\0';

    /* enabled 字段（显式关闭） */
    const char *en = strstr(buf, "\"enabled\"");
    if (en) {
        const char *colon = strchr(en, ':');
        if (colon) {
            while (*colon && *colon != 't' && *colon != 'f') colon++;
            if (colon[0] == 'f') g_user_disabled = 1;
        }
    }

    /* domains 数组 */
    const char *dp = strstr(buf, "\"domains\"");
    if (!dp) return;
    const char *lb = strchr(dp, '[');
    if (!lb) return;
    const char *p = lb + 1;
    while (*p) {
        if (*p == ']') break;
        if (*p == '"') {
            p++;
            char item[EGRESS_MAX_ITEM_LEN];
            size_t n = 0;
            while (*p && *p != '"' && n < sizeof(item) - 1) {
                if (*p == '\\' && p[1]) { p++; }
                item[n++] = *p++;
            }
            item[n] = '\0';
            /* 通配前缀 "*.x.com" → 存 "x.com"（后缀匹配天然覆盖子域） */
            if (n > 2 && item[0] == '*' && item[1] == '.') {
                egress_add(item + 2);
            } else {
                egress_add(item);
            }
            if (*p == '"') p++;
        } else {
            p++;
        }
    }
}

static void egress_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    for (int i = 0; g_default_allow[i]; i++) egress_add(g_default_allow[i]);
    egress_load_user_file();
    LOG_INFO_T("Egress", "Load", "OK", "allowlist loaded: %d entries (user_file=%s)",
               g_allow_count, g_user_disabled ? "disabled" : "merged");
}

void egress_reload(void) {
    g_loaded = 0;
    g_allow_count = 0;
    g_user_disabled = 0;
}

/* ============================================================
 * 判定
 * ============================================================ */

static int is_loopback_or_lan(const char *host) {
    if (!host || !*host) return 0;
    if (strcasecmp(host, "localhost") == 0) return 1;
    size_t hl = strlen(host);
    if (hl > 6 && strcasecmp(host + hl - 6, ".local") == 0) return 1;

    /* 127.* / ::1 */
    if (strncmp(host, "127.", 4) == 0) return 1;
    if (strcmp(host, "::1") == 0) return 1;

    /* 私有地址段 */
    if (strncmp(host, "10.", 3) == 0) return 1;
    if (strncmp(host, "192.168.", 8) == 0) return 1;
    if (strncmp(host, "172.", 4) == 0) {
        int second = atoi(host + 4);
        if (second >= 16 && second <= 31) return 1;
    }
    if (strncasecmp(host, "fd", 2) == 0) return 1;       /* fd00::/8 ULA */
    if (strncasecmp(host, "fe80", 4) == 0) return 1;     /* 链路本地 IPv6（内网） */
    return 0;
}

static int is_link_local_metadata(const char *host) {
    /* 169.254.0.0/16 —— 链路本地/云元数据（SSRF 头号目标）→ 一律拒绝 */
    return (strncmp(host, "169.254.", 8) == 0);
}

static int match_allowlist(const char *host) {
    for (int i = 0; i < g_allow_count; i++) {
        const char *item = g_allow[i];
        size_t il = strlen(item);
        if (il == 0) continue;
        if (strcasecmp(host, item) == 0) return 1;
        size_t hl = strlen(host);
        if (hl > il && strcasecmp(host + (hl - il), item) == 0
            && host[hl - il - 1] == '.') {
            return 1;   /* 子域命中：a.example.com ← example.com */
        }
    }
    return 0;
}

int egress_check(const char *host) {
    if (!host || !*host) return -1;

    /* 选项关闭 → 全放行 */
    if (options_get && options_get("sec.egress_allowlist") == 0) return 0;

    egress_load();
    if (g_user_disabled) return 0;

    /* 链路本地/云元数据：先于一切放行规则拒绝 */
    if (is_link_local_metadata(host)) {
        LOG_WARN_T("Egress", "Check", "Blocked", "link-local/metadata host blocked (SSRF): %s", host);
        return -1;
    }

    /* 本机 / 局域网 → 放行 */
    if (is_loopback_or_lan(host)) return 0;

    /* 公网 → 白名单 */
    if (match_allowlist(host)) return 0;

    LOG_WARN_T("Egress", "Check", "Blocked",
               "host not in egress allowlist: %s（可加入 %s/system/config/egress_allowlist.json）",
               host, lingos_data_root());
    return -1;
}

int egress_check_url(const char *url) {
    if (!url) return -1;
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':') end++;
    size_t hl = (size_t)(end - p);
    if (hl == 0 || hl >= 256) return -1;
    char host[300];
    memcpy(host, p, hl);
    host[hl] = '\0';
    return egress_check(host);
}
