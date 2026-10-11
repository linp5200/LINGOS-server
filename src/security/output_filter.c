/**
 * @file    output_filter.c
 * @brief   输出过滤：打码敏感信息（IP、邮箱等），支持热加载配置
 * @version 2.1.0.0
 *
 * 【P5 接线上岗】此前零调用者。现在真实调用点：
 *   · src/security/audit.c: audit_dump() / audit_export_json() —— 对外导出前脱敏
 *   · src/security/security_wire.c: security_wire_redact_output()
 * 同时修复：替换串比原文长（7 字节 IP → 13 字节占位）会在长文本上写越界，
 *   现按容量上限保守停止替换。
 */

#include "output_filter.h"
#include "secure_memory.h"   /* 【P5 接线】脱敏工作区用锁定内存（原文含敏感内容） */
#include "../common/safe_string.h"   /* safe_strncpy（可重入版拷出用） */
#include "../common/string_no_sys.h"
#include "log_extra.h"
#include "../common/data_path.h"
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define REDACT_BUF_SIZE 8192

static char *secure_buf = NULL;                  /* 首选：锁定内存工作区（懒分配） */
static char  static_buf[REDACT_BUF_SIZE];        /* 降级：锁定失败时的静态工作区 */
static int   fallback_mode = 0;                  /* 1 = 已降级（避免每次调用重复尝试分配） */
static regex_t ip_regex;
static regex_t email_regex;
static int regex_compiled = 0;
static pthread_mutex_t g_redact_lock = PTHREAD_MUTEX_INITIALIZER;  /* 工作区/正则共享，串行化 */

/* 工作区：优先锁定内存，失败降级静态缓冲（保证脱敏永不中断） */
static char *redact_workspace(void) {
    if (secure_buf) return secure_buf;
    if (fallback_mode) return static_buf;

    secure_buf = secure_alloc(REDACT_BUF_SIZE);
    if (!secure_buf) {
        LOG_WARN_T("OutputFilter", "Workspace", "Fallback",
                   "secure_alloc(%d) failed, using static buffer", REDACT_BUF_SIZE);
        fallback_mode = 1;
        explicit_bzero(static_buf, sizeof(static_buf));
        return static_buf;
    }
    return secure_buf;
}

/* 释放工作区：热重载时清掉上一次的敏感原文 */
static void release_workspace(void) {
    if (secure_buf) {
        secure_free(secure_buf, REDACT_BUF_SIZE);
        secure_buf = NULL;
    }
    explicit_bzero(static_buf, sizeof(static_buf));
    fallback_mode = 0;   /* 下次重载重新尝试申请锁定内存 */
}

static void compile_regexes(void) {
    if (regex_compiled) {
        regfree(&ip_regex);
        regfree(&email_regex);
        regex_compiled = 0;
    }
    if (regcomp(&ip_regex, "\\b([0-9]{1,3}\\.){3}[0-9]{1,3}\\b", REG_EXTENDED) != 0) {
        LOG_ERROR_T("OutputFilter", "Compile", "IPRegexFail", "regcomp(ip) failed");
        return;
    }
    if (regcomp(&email_regex, "\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Z|a-z]{2,}\\b", REG_EXTENDED) != 0) {
        LOG_ERROR_T("OutputFilter", "Compile", "EmailRegexFail", "regcomp(email) failed");
        regfree(&ip_regex);
        return;
    }
    regex_compiled = 1;
}

/* 全量替换：按容量上限保守处理，杜绝「短串换长串」导致的写越界 */
static void replace_all(char *buf, size_t cap, regex_t *re, const char *rep) {
    size_t rep_len = strlen(rep);
    char *p = buf;

    for (;;) {
        regmatch_t m[1];
        if (regexec(re, p, 1, m, 0) != 0) break;

        size_t start = (size_t)m[0].rm_so;
        size_t end = (size_t)m[0].rm_eo;
        if (end <= start) {                 /* 零长度匹配：前进 1 字节，防死循环 */
            if (p[start] == '\0') break;
            p += start + 1;
            continue;
        }

        size_t len = strlen(buf);
        size_t new_len = len - (end - start) + rep_len;
        if (new_len + 1 > cap) {
            LOG_WARN_T("OutputFilter", "Redact", "Truncate",
                       "replacement would exceed %zu bytes, stop at offset %zu", cap, (size_t)(p - buf + start));
            break;
        }

        memmove(p + start + rep_len, p + end, len - end + 1);
        memcpy(p + start, rep, rep_len);
        p += start + rep_len;
    }
}

/* 热重载（重新编译正则 + 清空上一次的敏感原文）*/
int output_filter_reload(void) {
    release_workspace();
    compile_regexes();
    LOG_INFO_T("OutputFilter", "Reload", "OK", "regex patterns reloaded");
    return regex_compiled ? 0 : -1;
}

/* 【P5 接线】可重入版：内部加锁 + 结果拷入调用方缓冲。
 *   audit.c（lingosd 线程）与 security_wire.c（handler 线程）并发调用安全。 */
int output_filter_redact_r(const char *output, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    if (!output) return 0;

    pthread_mutex_lock(&g_redact_lock);
    if (!regex_compiled) compile_regexes();

    char *buf = redact_workspace();
    strncpy(buf, output, REDACT_BUF_SIZE - 1);
    buf[REDACT_BUF_SIZE - 1] = '\0';

    if (regex_compiled) {
        replace_all(buf, REDACT_BUF_SIZE, &ip_regex, "[REDACTED_IP]");
        replace_all(buf, REDACT_BUF_SIZE, &email_regex, "[REDACTED_EMAIL]");
    } else {
        LOG_WARN_T("OutputFilter", "Redact", "NoRegex", "regex unavailable, output passed through");
    }

    safe_strncpy(out, buf, out_sz);
    pthread_mutex_unlock(&g_redact_lock);

    LOG_DEBUG_T("OutputFilter", "Redact", "Done", "output filtered (reentrant)");
    return 0;
}

const char *output_filter_redact(const char *output) {
    if (!output) return "";
    if (!regex_compiled) compile_regexes();

    char *buf = redact_workspace();
    strncpy(buf, output, REDACT_BUF_SIZE - 1);
    buf[REDACT_BUF_SIZE - 1] = '\0';

    if (regex_compiled) {
        replace_all(buf, REDACT_BUF_SIZE, &ip_regex, "[REDACTED_IP]");
        replace_all(buf, REDACT_BUF_SIZE, &email_regex, "[REDACTED_EMAIL]");
    } else {
        LOG_WARN_T("OutputFilter", "Redact", "NoRegex", "regex unavailable, output passed through");
    }

    LOG_DEBUG_T("OutputFilter", "Redact", "Done", "output filtered");
    return buf;
}
