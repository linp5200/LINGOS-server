/**
 * @file    input_filter.c
 * @brief   输入过滤：检测危险命令模式，支持热加载配置
 * @version 2.0.0.1
 */

#include "input_filter.h"
#include "../common/string_no_sys.h"
#include "log_extra.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char **patterns = NULL;
static int pattern_count = 0;

/* 默认内置模式 */
static const char *default_patterns[] = {
    "rm -rf /",
    "rm -rf /*",
    "mkfs",
    "dd if=/dev/zero",
    "> /dev/sda",
    "drop table",
    "truncate table",
    "eval(",
    "exec(",
    "system(",
    "popen(",
    "; rm ",
    "| sh",
    "`",
    "$(",
    NULL
};

static void free_patterns(void) {
    if (patterns) {
        for (int i = 0; i < pattern_count; i++) {
            free(patterns[i]);
        }
        free(patterns);
        patterns = NULL;
        pattern_count = 0;
    }
}

static int load_patterns_from_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    int count = 0;
    char **tmp = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        tmp = realloc(tmp, (count+1) * sizeof(char*));
        if (!tmp) { fclose(fp); return -1; }
        tmp[count] = strdup(line);
        if (!tmp[count]) { fclose(fp); return -1; }
        count++;
    }
    fclose(fp);
    free_patterns();
    patterns = tmp;
    pattern_count = count;
    LOG_INFO_T("InputFilter", "Load", "OK", "loaded %d patterns from %s", count, path);
    return 0;
}

static void load_default_patterns(void) {
    free_patterns();
    for (int i = 0; default_patterns[i]; i++) {
        patterns = realloc(patterns, (pattern_count+1) * sizeof(char*));
        if (patterns) {
            patterns[pattern_count] = strdup(default_patterns[i]);
            pattern_count++;
        }
    }
    LOG_INFO_T("InputFilter", "Load", "Default", "loaded %d default patterns", pattern_count);
}

/* 初始化：尝试从配置文件加载，否则用默认 */
void input_filter_init(void) {
    const char *root = lingos_data_root();
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/system/config/filters.conf", root);
    if (load_patterns_from_file(config_path) != 0) {
        load_default_patterns();
    }
}

/* 热重载接口 */
int input_filter_reload(void) {
    const char *root = lingos_data_root();
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/system/config/filters.conf", root);
    if (load_patterns_from_file(config_path) == 0) {
        LOG_INFO_T("InputFilter", "Reload", "OK", "reloaded patterns");
        return 0;
    }
    LOG_WARN_T("InputFilter", "Reload", "Fail", "using existing patterns");
    return -1;
}

int input_filter_check(const char *input, char *reason, uint32_t reason_len) {
    if (!input) return 0;
    if (!patterns) input_filter_init();

    for (int i = 0; i < pattern_count; i++) {
        if (strstr(input, patterns[i])) {
            snprintf(reason, reason_len, "Dangerous pattern detected: %s", patterns[i]);
            LOG_WARN_T("InputFilter", "Block", "Pattern", "%s", patterns[i]);
            return -1;
        }
    }
    if (strlen(input) > 4096) {
        snprintf(reason, reason_len, "Input too long (>4096 chars)");
        LOG_WARN_T("InputFilter", "Block", "TooLong", "len=%zu", strlen(input));
        return -1;
    }
    return 0;
}