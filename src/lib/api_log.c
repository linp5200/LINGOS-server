/**
 * @file    src/lib/api_log.c
 * @brief   API 日志 + 设备修改日志实现（v0.7.2 格式升级 · 方案 B 语义映射）
 * @version LN-0.7.2
 */

#include "api_log.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* options_get 弱符号（无 options 的二进制 → 默认开启） */
extern int options_get(const char *key) __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *log_dir_path(void) {
    static char p[512];
    if (p[0] == '\0') {
        safe_snprintf(p, sizeof(p), "%s/log", lingos_data_root());
    }
    return p;
}

static void now_str(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void append_line(const char *file, const char *line) {
    char path[600];
    safe_snprintf(path, sizeof(path), "%s/%s", log_dir_path(), file);
    pthread_mutex_lock(&g_lock);
    FILE *fp = fopen(path, "a");
    if (fp) {
        fputs(line, fp);
        fputc('\n', fp);
        fflush(fp);   /* 防关机丢失（先生设定："不被清除"之防关机） */
        fclose(fp);
    }
    pthread_mutex_unlock(&g_lock);
}

/* ============================================================
 * 大小格式化（人类可读：84B / 2.1KB / 1.2MB）
 * ============================================================ */
static void fmt_size(long n, char *out, size_t out_sz) {
    if (n < 0) n = 0;
    if (n < 1024) {
        safe_snprintf(out, out_sz, "%ldB", n);
    } else if (n < 1024 * 1024) {
        safe_snprintf(out, out_sz, "%.1fKB", n / 1024.0);
    } else {
        safe_snprintf(out, out_sz, "%.1fMB", n / (1024.0 * 1024.0));
    }
}

/* ============================================================
 * 敏感打码（token / password / key / secret … 的值 → ***）
 *   隐私红线（先生设定）：落盘日志中凭据不得明文。
 *   策略：定位关键词 → 其后 24 字符内找 : 或 = → 值段（≥8 字符）整体替换。
 * ============================================================ */
static void redact_inplace(char *s) {
    static const char *keys[] = {
        "token", "password", "passwd", "api_key", "apikey", "api-key",
        "secret", "authorization", "credential", "private_key", NULL
    };
    if (!s || !*s) return;
    for (int k = 0; keys[k]; k++) {
        size_t kl = strlen(keys[k]);
        char *p = s;
        while ((p = strcasestr(p, keys[k])) != NULL) {
            /* 在关键词后 24 字符内寻找 ':' 或 '=' */
            char *q = p + kl;
            char *lim = p + kl + 24;
            char *sep = NULL;
            for (char *t = q; *t && t < lim; t++) {
                if (*t == ':' || *t == '=') { sep = t; break; }
                if (*t == '"' || *t == '\\' || *t == ',') break;   /* 关键词与值分隔过远 */
            }
            if (!sep) { p += kl; continue; }
            /* 跳过分隔符与空白/引号 */
            char *v = sep + 1;
            while (*v == ' ' || *v == '\t') v++;
            int quoted = 0;
            if (*v == '"') { quoted = 1; v++; }
            /* 找值尾 */
            char *ve = v;
            while (*ve && *ve != ',' && *ve != '}' && *ve != '&' && *ve != ' ' &&
                   *ve != '"' && *ve != '\n' && *ve != '\r') ve++;
            size_t vlen = (size_t)(ve - v);
            if (vlen >= 8) {
                /* token 类保留前 4 位便于审计（与 S16 "前 8 位"精神一致，日志更短用 4） */
                char keep[8] = {0};
                for (size_t i = 0; i < 4 && i < vlen; i++) keep[i] = v[i];
                char repl[32];
                safe_snprintf(repl, sizeof(repl), "%s***", keep);
                size_t rl = strlen(repl);
                memmove(v + rl, ve, strlen(ve) + 1);
                memcpy(v, repl, rl);
                (void)quoted;
                p = v + rl;
            } else {
                p = v;
            }
        }
    }
}

/* ============================================================
 * 摘要生成（打码 + 截断 + 单行化）
 * ============================================================ */
static void summarize(const char *raw, size_t raw_len,
                      char *out, size_t out_sz) {
    if (!raw || !*raw) {
        safe_snprintf(out, out_sz, "-");
        return;
    }
    size_t n = raw_len ? raw_len : strlen(raw);
    size_t take = n < (size_t)API_SUMMARY_MAX ? n : (size_t)API_SUMMARY_MAX;
    char buf[API_SUMMARY_MAX * 2 + 8];
    size_t j = 0;
    for (size_t i = 0; i < take && j < sizeof(buf) - 8; i++) {
        char c = raw[i];
        if (c == '\n' || c == '\r' || c == '\t') { buf[j++] = ' '; continue; }
        if (c == '"') { /* JSON 引号转义——日志内使用单引号显示 */ buf[j++] = '\''; continue; }
        if (c == '\\') { buf[j++] = '\\'; buf[j++] = '\\'; continue; }
        buf[j++] = c;
    }
    if (n > take) {
        buf[j++] = '.'; buf[j++] = '.';
    }
    buf[j] = '\0';
    redact_inplace(buf);
    safe_strncpy(out, buf, out_sz);
}

/* ============================================================
 * 方法映射（方案 B：命令名语义 → GET/POST/DELETE）
 * ============================================================ */
static int contains_ci(const char *hay, const char *needle) {
    return hay && needle && strcasestr(hay, needle) != NULL;
}

const char *api_log_method(const char *op, const char *http_method) {
    if (http_method && *http_method) return http_method;   /* HTTP 通道：真实方法 */
    if (!op || !*op) return "POST";

    /* 删除语义 */
    static const char *del_words[] = {
        "delete", "remove", "uninstall", "kill", "drop", "revoke", "clear", NULL
    };
    for (int i = 0; del_words[i]; i++) {
        if (contains_ci(op, del_words[i])) return "DELETE";
    }
    /* 查询语义 */
    static const char *get_words[] = {
        "list", "query", "status", "info", "search", "read", "current",
        "forecast", "summary", "overview", "find", "show", "check", "scan",
        "get", "ping", "discovery_scan", "list", NULL
    };
    for (int i = 0; get_words[i]; i++) {
        if (contains_ci(op, get_words[i])) return "GET";
    }
    return "POST";
}

/* ============================================================
 * 类别映射（命令前缀/URL 前缀 → 业务类别）
 * ============================================================ */
const char *api_log_category(const char *op) {
    if (!op || !*op) return "Misc";
    /* 命令名前缀表（顺序敏感：先长后短） */
    static const struct { const char *prefix; const char *cat; } CATS[] = {
        /* AI 对话 */
        {"chat",         "AIChat"},   {"nook",        "AIChat"},
        {"agent",        "AIChat"},   {"sub_ai",      "AIChat"},
        {"summarize",    "AIChat"},   {"react",       "AIChat"},
        /* 服务器模式 */
        {"server_mode",  "ServerMode"},
        /* 遥测 */
        {"system_info",  "Telemetry"},{"system_cpu",  "Telemetry"},
        {"system_memory","Telemetry"},{"system_disk", "Telemetry"},
        {"system_uptime","Telemetry"},{"system_",     "Telemetry"},
        {"net_status",   "Telemetry"},{"net_ping",    "Telemetry"},
        {"net_dns",      "Telemetry"},{"net_curl",    "Telemetry"},
        {"port_list",    "Telemetry"},{"command_list","Telemetry"},
        {"process_",     "Telemetry"},{"service_",    "Telemetry"},
        /* 文件 / 记忆 / 会话 */
        {"file_",        "FileOp"},   {"script_",     "FileOp"},
        {"memory_",      "Memory"},   {"session_",    "Session"},
        /* 预警与天气 */
        {"alert_",       "Alert"},    {"weather_",    "Weather"},
        {"typhoon",      "Alert"},    {"earthquake_", "Alert"},
        /* 通知 */
        {"notify",       "Notify"},
        /* 认证/权限/加密 */
        {"auth_",        "Auth"},     {"permission_", "Auth"},
        {"crypto_",      "Crypto"},   {"privacy_",    "Crypto"},
        /* 技能/选项/更新 */
        {"skill",        "Skill"},    {"skills",      "Skill"},
        {"options_",     "Options"},  {"update",      "Update"},
        /* 视觉/监控 */
        {"monitor",      "Vision"},   {"vision",      "Vision"},
        {"yolo",         "Vision"},   {"ocr",         "Vision"},
        {"nvr_",         "Vision"},   {"timeline",    "Vision"},
        {"storage_",     "Vision"},   {"media_",      "Vision"},
        {"rtsp",         "Vision"},   {"camera_",     "Vision"},
        /* 智能家居 */
        {"ha_",          "Home"},     {"entity_",     "Home"},
        {"scene_",       "Home"},     {"area_",       "Home"},
        {"label_",       "Home"},     {"zone_",       "Home"},
        {"presence_",    "Home"},     {"energy_",     "Home"},
        {"webhook",      "Home"},     {"blueprint_",  "Home"},
        {"discovery_",   "Home"},     {"mqtt",        "Home"},
        /* 危机 */
        {"crisis",       "Crisis"},
        /* 语音 */
        {"voice_",       "Voice"},    {"tts",         "Voice"}, {"stt", "Voice"},
        /* HTTP 路径 */
        {"/api/cmd",     "Command"},  {"/api/files",  "FileOp"},
        {"/api/webhook", "Home"},     {"/api/audio",  "Voice"},
        {"/system/health","Telemetry"},{"/nook/ask",  "AIChat"},
        {"/ui",          "UI"},       {"/console",    "UI"},
        {NULL, NULL}
    };
    for (int i = 0; CATS[i].prefix; i++) {
        if (strncasecmp(op, CATS[i].prefix, strlen(CATS[i].prefix)) == 0) {
            return CATS[i].cat;
        }
    }
    return "Misc";
}

/* ============================================================
 * API 日志（7 列格式）
 * ============================================================ */
void api_log(const char *method, const char *channel, const char *device,
             int status, const char *op,
             const char *req, size_t req_len,
             const char *resp, size_t resp_len) {
    /* 开关：dev.api_log（0.x 默认开；显式 0 → 关闭） */
    if (options_get && options_get("dev.api_log") == 0) return;

    char ts[32];
    now_str(ts, sizeof(ts));

    const char *m = api_log_method(op, method);
    const char *cat = api_log_category(op);

    /* 通道:设备 */
    char chdev[96];
    if (device && *device && strcmp(device, "-") != 0) {
        safe_snprintf(chdev, sizeof(chdev), "%s:%s", channel ? channel : "-", device);
    } else {
        safe_snprintf(chdev, sizeof(chdev), "%s", channel ? channel : "-");
    }

    /* 请求/响应摘要 */
    char rs[API_SUMMARY_MAX * 2], ps[API_SUMMARY_MAX * 2];
    char rsz[16], psz[16];
    long rl = req_len ? (long)req_len : (req && *req ? (long)strlen(req) : 0);
    long pl = resp_len ? (long)resp_len : (resp && *resp ? (long)strlen(resp) : 0);
    summarize(req, rl, rs, sizeof(rs));
    summarize(resp, pl, ps, sizeof(ps));
    fmt_size(rl, rsz, sizeof(rsz));
    fmt_size(pl, psz, sizeof(psz));

    /* 状态显示：-1/0 → "-"（仅请求无响应） */
    char st[16];
    if (status > 0) safe_snprintf(st, sizeof(st), "%d", status);
    else safe_snprintf(st, sizeof(st), "-");

    char line[1024];
    safe_snprintf(line, sizeof(line),
        "%s | %-6s | %-16s | %-4s | %-10s | req{%s} \"%s\" | resp{%s} \"%s\"",
        ts, m, chdev, st, cat, rsz, rs, psz, ps);
    append_line("api.log", line);
}

/* ============================================================
 * 设备修改日志（保持原格式——审计专用）
 * ============================================================ */
static unsigned int g_req_seq = 0;

void devmod_new_req_id(char *out, unsigned int out_sz) {
    if (!out || out_sz == 0) return;
    unsigned int seq;
    pthread_mutex_lock(&g_lock);
    seq = ++g_req_seq;
    pthread_mutex_unlock(&g_lock);
    safe_snprintf(out, out_sz, "dm%lx%x", (unsigned long)time(NULL), seq & 0xffff);
}

void devmod_log(const char *op, const char *target, const char *req_id,
                int phase, long size, int sensitive, const char *result) {
    char ts[32];
    now_str(ts, sizeof(ts));

    /* 隐私：敏感 → 仅记长度（"摘要或长度"——不记原文） */
    char tgt[420];
    if (sensitive) {
        safe_snprintf(tgt, sizeof(tgt), "(敏感——长度 %ldB)", size);
    } else {
        safe_snprintf(tgt, sizeof(tgt), "%s", (target && *target) ? target : "-");
    }

    char line[1024];
    if (phase == 0) {
        safe_snprintf(line, sizeof(line),
            "%s [DEVMOD] op=%-6s phase=request req=%s target=%s size=%ldB",
            ts, (op && *op) ? op : "MOD", (req_id && *req_id) ? req_id : "-", tgt, size);
    } else {
        safe_snprintf(line, sizeof(line),
            "%s [DEVMOD] op=%-6s phase=done    req=%s result=%s size=%ldB",
            ts, (op && *op) ? op : "MOD", (req_id && *req_id) ? req_id : "-",
            (result && *result) ? result : "ok", size);
    }
    append_line("device_mod.log", line);
}

int log_path_protected(const char *path) {
    if (!path || !*path) return 0;

    /* 规范化前缀匹配：{root}/log 下的一切 + 直接写 log 目录名 */
    char prefix[512];
    safe_snprintf(prefix, sizeof(prefix), "%s/log", lingos_data_root());

    if (strncmp(path, prefix, strlen(prefix)) == 0) return 1;
    if (strstr(path, "/LINGOS/log") == path || strcmp(path, "/LINGOS/log") == 0) return 1;
    if (strstr(path, "/LINGOS/log/") != NULL) return 1;

    return 0;
}
