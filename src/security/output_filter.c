/**
 * @file    output_filter.c
 * @brief   输出过滤：打码敏感信息（IP、邮箱等），支持热加载配置
 * @version 2.0.0.1
 */

#include "output_filter.h"
#include "../common/string_no_sys.h"
#include "log_extra.h"
#include "../common/data_path.h"
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char redacted_buf[8192];
static regex_t ip_regex;
static regex_t email_regex;
static int regex_compiled = 0;

static void compile_regexes(void) {
    if (regex_compiled) {
        regfree(&ip_regex);
        regfree(&email_regex);
    }
    regcomp(&ip_regex, "\\b([0-9]{1,3}\\.){3}[0-9]{1,3}\\b", REG_EXTENDED);
    regcomp(&email_regex, "\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Z|a-z]{2,}\\b", REG_EXTENDED);
    regex_compiled = 1;
}

/* 热重载（重新编译正则）*/
int output_filter_reload(void) {
    compile_regexes();
    LOG_INFO_T("OutputFilter", "Reload", "OK", "regex patterns reloaded");
    return 0;
}

const char *output_filter_redact(const char *output) {
    if (!output) return "";
    if (!regex_compiled) compile_regexes();

    strncpy(redacted_buf, output, sizeof(redacted_buf)-1);
    redacted_buf[sizeof(redacted_buf)-1] = '\0';

    // 打码 IPv4 地址
    regmatch_t pmatch[1];
    char *p = redacted_buf;
    while (regexec(&ip_regex, p, 1, pmatch, 0) == 0) {
        int start = pmatch[0].rm_so;
        int end = pmatch[0].rm_eo;
        int len = end - start;
        if (len > 0) {
            memcpy(p + start, "[REDACTED_IP]", 13);
            int remain = strlen(p + end);
            memmove(p + start + 13, p + end, remain + 1);
            p += start + 13;
        } else break;
    }

    // 打码邮箱
    p = redacted_buf;
    while (regexec(&email_regex, p, 1, pmatch, 0) == 0) {
        int start = pmatch[0].rm_so;
        int end = pmatch[0].rm_eo;
        int len = end - start;
        if (len > 0) {
            memcpy(p + start, "[REDACTED_EMAIL]", 16);
            int remain = strlen(p + end);
            memmove(p + start + 16, p + end, remain + 1);
            p += start + 16;
        } else break;
    }

    LOG_DEBUG_T("OutputFilter", "Redact", "Done", "output filtered");
    return redacted_buf;
}