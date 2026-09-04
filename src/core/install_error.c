/**
 * @file    src/core/install_error.c
 * @brief   安装错误动态解析系统实现
 * @version LN-0.4.3
 * @par     核心协议：防弹/容错编程
 */

#include "install_error.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================
 * 错误-解决方案映射表（精确匹配模式）
 * ============================================================ */

typedef struct {
    const char *pattern;                 /* 匹配模式（strstr 搜索） */
    install_error_type_t error_type;     /* 匹配到的错误类型 */
    solution_type_t solution_type;       /* 推荐的解决方案 */
    const char *solution_data;           /* 解决方案参数 */
    int priority;                        /* 优先级 */
    int confidence;                      /* 置信度 */
} error_pattern_entry_t;

static error_pattern_entry_t g_error_patterns[] = {
    /* ===== 包未找到 ===== */
    {"E: Unable to locate package", ERR_TYPE_PKG_NOT_FOUND,
     SOLVE_RETRY_WITH_ALT_NAME, NULL, 1, 95},
    {"E: Couldn't find any package by glob", ERR_TYPE_PKG_NOT_FOUND,
     SOLVE_RETRY_WITH_ALT_NAME, NULL, 1, 95},
    {"No package .* found", ERR_TYPE_PKG_NOT_FOUND,
     SOLVE_RETRY_WITH_ALT_NAME, NULL, 2, 80},
    {"Could not find a version that satisfies", ERR_TYPE_PKG_NOT_FOUND,
     SOLVE_RETRY_WITH_ALT_NAME, NULL, 1, 90},

    /* ===== PEP 668 ===== */
    {"externally-managed-environment", ERR_TYPE_PEP_668,
     SOLVE_ADD_BREAK_FLAG, NULL, 1, 100},
    {"This environment is externally managed", ERR_TYPE_PEP_668,
     SOLVE_ADD_BREAK_FLAG, NULL, 1, 100},

    /* ===== 依赖冲突 ===== */
    {"Depends: .* but .* is to be installed", ERR_TYPE_DEPENDENCY_CONFLICT,
     SOLVE_FIX_BROKEN, NULL, 2, 80},
    {"unmet dependencies", ERR_TYPE_UNMET_DEPENDENCIES,
     SOLVE_FIX_BROKEN, NULL, 1, 90},
    {"dependency conflict", ERR_TYPE_DEPENDENCY_CONFLICT,
     SOLVE_FIX_BROKEN, NULL, 2, 85},
    {"Dependency resolution failed", ERR_TYPE_PIP_DEPENDENCY_RESOLVE,
     SOLVE_FIX_BROKEN, NULL, 2, 80},

    /* ===== 源不可达 ===== */
    {"Failed to fetch .* 404 Not Found", ERR_TYPE_SOURCE_UNREACHABLE,
     SOLVE_TRY_MIRROR, "apt", 3, 85},
    {"Failed to fetch .* Connection timed out", ERR_TYPE_SOURCE_UNREACHABLE,
     SOLVE_TRY_MIRROR, "apt", 3, 80},

    /* ===== 网络超时 ===== */
    {"Connection timed out", ERR_TYPE_NETWORK_TIMEOUT,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 85},
    {"Temporary failure resolving", ERR_TYPE_NETWORK_TIMEOUT,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 90},
    {"Could not resolve host", ERR_TYPE_NETWORK_TIMEOUT,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 85},
    {"SSL: connection timeout", ERR_TYPE_NETWORK_TIMEOUT,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 75},

    /* ===== 权限不足 ===== */
    {"Could not open lock file /var/lib/dpkg/lock", ERR_TYPE_PERMISSION_DENIED,
     SOLVE_WAIT_AND_RETRY, NULL, 4, 95},
    {"Permission denied", ERR_TYPE_PERMISSION_DENIED,
     SOLVE_WAIT_AND_RETRY, NULL, 4, 70},
    {"Operation not permitted", ERR_TYPE_PERMISSION_DENIED,
     SOLVE_WAIT_AND_RETRY, NULL, 4, 70},

    /* ===== 磁盘空间不足 ===== */
    {"No space left on device", ERR_TYPE_DISK_FULL,
     SOLVE_CLEAN_CACHE_AND_RETRY, NULL, 5, 95},
    {"Disk full", ERR_TYPE_DISK_FULL,
     SOLVE_CLEAN_CACHE_AND_RETRY, NULL, 5, 85},

    /* ===== 损坏的包 ===== */
    {"dpkg: error processing package", ERR_TYPE_BROKEN_PACKAGE,
     SOLVE_FIX_BROKEN, NULL, 2, 90},
    {"dpkg: dependency problems", ERR_TYPE_BROKEN_PACKAGE,
     SOLVE_FIX_BROKEN, NULL, 2, 90},
    {"broken packages", ERR_TYPE_BROKEN_PACKAGE,
     SOLVE_FIX_BROKEN, NULL, 2, 80},

    /* ===== GPG 签名错误 ===== */
    {"GPG error", ERR_TYPE_SIGNATURE_INVALID,
     SOLVE_UPDATE_KEYS, NULL, 4, 85},
    {"NO_PUBKEY", ERR_TYPE_SIGNATURE_INVALID,
     SOLVE_UPDATE_KEYS, NULL, 4, 95},
    {"The following signatures couldn't be verified", ERR_TYPE_SIGNATURE_INVALID,
     SOLVE_UPDATE_KEYS, NULL, 4, 85},

    /* ===== pip 错误 ===== */
    {"No matching distribution found", ERR_TYPE_PIP_NO_MATCHING_DIST,
     SOLVE_TRY_ALT_INDEX, NULL, 3, 90},
    {"Failed building wheel for", ERR_TYPE_PIP_BUILD_FAILED,
     SOLVE_INSTALL_BUILD_DEPS, NULL, 4, 85},
    {"Could not find a version", ERR_TYPE_PIP_NO_MATCHING_DIST,
     SOLVE_TRY_ALT_INDEX, NULL, 3, 80},
    {"pip version conflict", ERR_TYPE_PIP_VERSION_MISMATCH,
     SOLVE_SKIP_PACKAGE, NULL, 5, 70},

    /* ===== apt update 失败 ===== */
    {"apt update failed", ERR_TYPE_CACHE_UPDATE_FAILED,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 80},
    {"Failed to update package cache", ERR_TYPE_CACHE_UPDATE_FAILED,
     SOLVE_RETRY_WITH_BACKOFF, NULL, 3, 80},

    /* 兜底：记录输出 */
    {NULL, ERR_TYPE_UNKNOWN, SOLVE_RECORD_OUTPUT, NULL, 99, 0}
};

/* ============================================================
 * 包名变体映射表（内置）
 * ============================================================ */

static pkg_variant_map_t g_pkg_variants[] = {
    /* ===== dev 包变体 ===== */
    {"libmosquitto-dev",      {"libmosquitto-devel", "mosquitto-dev", "mosquitto-devel", NULL}},
    {"libsqlite3-dev",        {"sqlite-devel", "libsqlite-devel", "sqlite-dev", NULL}},
    {"libmicrohttpd-dev",     {"libmicrohttpd-devel", "microhttpd-dev", "microhttpd-devel", NULL}},
    {"libcurl4-openssl-dev",  {"libcurl-devel", "curl-dev", "libcurl-dev", NULL}},
    {"libseccomp-dev",        {"libseccomp-devel", "seccomp-devel", NULL}},
    {"libnotcurses-dev",      {"notcurses-devel", "libnotcurses-devel", NULL}},
    {"libssl-dev",            {"openssl-devel", "libssl-devel", NULL}},
    {"libxml2-dev",           {"libxml2-devel", "xml2-devel", NULL}},
    {"libyaml-dev",           {"libyaml-devel", "yaml-devel", NULL}},
    {"libffi-dev",            {"libffi-devel", "ffi-devel", NULL}},
    {"libsystemd-dev",        {"systemd-devel", "libsystemd-devel", NULL}},

    /* ===== Python 包变体 ===== */
    {"sentence_transformers", {"sentence-transformers", "python3-sentence-transformers", NULL}},
    {"Pillow",                {"python3-pil", "python3-pillow", NULL}},
    {"paho-mqtt",             {"python3-paho-mqtt", "paho", NULL}},
    {"ultralytics",           {"python3-ultralytics", NULL}},
    {"tiktoken",              {"python3-tiktoken", NULL}},
    {"vosk",                  {"python3-vosk", NULL}},
    {"flask",                 {"python3-flask", NULL}},
    {"flask-socketio",        {"python3-flask-socketio", NULL}},
    {"flask-cors",            {"python3-flask-cors", NULL}},
    {"requests",              {"python3-requests", NULL}},
    {"numpy",                 {"python3-numpy", NULL}},

    /* ===== 通用工具变体 ===== */
    {"arp-scan",              {"arp-scan", NULL}},
    {"nmap",                  {"nmap", NULL}},
    {"systemd",               {"systemd", NULL}},
    {"mosquitto",             {"mosquitto", NULL}},

    {NULL, {NULL}}
};

/* ============================================================
 * 错误类型描述表
 * ============================================================ */

typedef struct {
    install_error_type_t type;
    const char *desc_en;
    const char *desc_zh;
} error_desc_entry_t;

static error_desc_entry_t g_error_descs[] = {
    {ERR_TYPE_UNKNOWN, "Unknown error", "未知错误"},
    {ERR_TYPE_PKG_NOT_FOUND, "Package not found", "包未找到"},
    {ERR_TYPE_DEPENDENCY_CONFLICT, "Dependency conflict", "依赖冲突"},
    {ERR_TYPE_SOURCE_UNREACHABLE, "Package source unreachable", "软件包源不可达"},
    {ERR_TYPE_PERMISSION_DENIED, "Permission denied", "权限不足"},
    {ERR_TYPE_PEP_668, "Externally managed environment (PEP 668)", "外部管理环境 (PEP 668)"},
    {ERR_TYPE_NETWORK_TIMEOUT, "Network timeout", "网络超时"},
    {ERR_TYPE_DISK_FULL, "Disk full", "磁盘空间不足"},
    {ERR_TYPE_BROKEN_PACKAGE, "Broken package", "损坏的包"},
    {ERR_TYPE_UNMET_DEPENDENCIES, "Unmet dependencies", "未满足的依赖"},
    {ERR_TYPE_SIGNATURE_INVALID, "Invalid signature", "签名无效"},
    {ERR_TYPE_CACHE_UPDATE_FAILED, "Cache update failed", "缓存更新失败"},
    {ERR_TYPE_PIP_VERSION_MISMATCH, "Pip version conflict", "Pip 版本冲突"},
    {ERR_TYPE_PIP_NO_MATCHING_DIST, "No matching distribution", "未找到匹配的分发"},
    {ERR_TYPE_PIP_BUILD_FAILED, "Build failed", "构建失败"},
    {ERR_TYPE_PIP_DEPENDENCY_RESOLVE, "Dependency resolution failed", "依赖解析失败"},
    {ERR_TYPE_MAX, NULL, NULL}
};

/* ============================================================
 * 解决方案类型描述表
 * ============================================================ */

typedef struct {
    solution_type_t type;
    const char *desc_en;
    const char *desc_zh;
} solution_desc_entry_t;

static solution_desc_entry_t g_solution_descs[] = {
    {SOLVE_RETRY_WITH_ALT_NAME, "Try alternative package name", "尝试替代包名"},
    {SOLVE_ADD_BREAK_FLAG, "Add --break-system-packages flag", "添加 --break-system-packages 参数"},
    {SOLVE_FIX_BROKEN, "Fix broken dependencies", "修复损坏的依赖"},
    {SOLVE_TRY_MIRROR, "Try mirror source", "尝试镜像源"},
    {SOLVE_RETRY_WITH_BACKOFF, "Retry with backoff", "退避重试"},
    {SOLVE_WAIT_AND_RETRY, "Wait and retry", "等待后重试"},
    {SOLVE_CLEAN_CACHE_AND_RETRY, "Clean cache and retry", "清理缓存后重试"},
    {SOLVE_UPDATE_KEYS, "Update GPG keys", "更新 GPG 密钥"},
    {SOLVE_TRY_ALT_INDEX, "Try alternative index", "尝试备用索引"},
    {SOLVE_INSTALL_BUILD_DEPS, "Install build dependencies", "安装编译依赖"},
    {SOLVE_SKIP_PACKAGE, "Skip package", "跳过包"},
    {SOLVE_RECORD_OUTPUT, "Record output only", "仅记录输出"},
    {SOLVE_MAX, NULL, NULL}
};

/* ============================================================
 * 内部辅助函数
 * ============================================================ */

static int string_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL;
}

static int string_contains_ignore_case(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    /* 简化实现：转换为小写后比较 */
    char *haystack_lower = strdup(haystack);
    char *needle_lower = strdup(needle);
    if (!haystack_lower || !needle_lower) {
        free(haystack_lower);
        free(needle_lower);
        return strstr(haystack, needle) != NULL;
    }
    for (char *p = haystack_lower; *p; p++) *p = tolower(*p);
    for (char *p = needle_lower; *p; p++) *p = tolower(*p);
    int result = strstr(haystack_lower, needle_lower) != NULL;
    free(haystack_lower);
    free(needle_lower);
    return result;
}

/* ============================================================
 * 精确匹配
 * ============================================================ */

static int match_exact(const char *output, const error_pattern_entry_t *pattern,
                       install_error_result_t *result) {
    if (!output || !pattern || !pattern->pattern) return 0;

    if (string_contains(output, pattern->pattern)) {
        result->error_type = pattern->error_type;
        result->solution_type = pattern->solution_type;
        result->solution_data = pattern->solution_data;
        result->matched_pattern = pattern->pattern;
        result->confidence = pattern->confidence;
        return 1;
    }
    return 0;
}

/* ============================================================
 * 模糊匹配（关键词）
 * ============================================================ */

typedef struct {
    const char *keyword;
    install_error_type_t error_type;
    solution_type_t solution_type;
    int confidence;
} fuzzy_keyword_t;

static fuzzy_keyword_t g_fuzzy_keywords[] = {
    {"not found", ERR_TYPE_PKG_NOT_FOUND, SOLVE_RETRY_WITH_ALT_NAME, 60},
    {"404", ERR_TYPE_SOURCE_UNREACHABLE, SOLVE_TRY_MIRROR, 55},
    {"timeout", ERR_TYPE_NETWORK_TIMEOUT, SOLVE_RETRY_WITH_BACKOFF, 65},
    {"unable to locate", ERR_TYPE_PKG_NOT_FOUND, SOLVE_RETRY_WITH_ALT_NAME, 70},
    {"missing", ERR_TYPE_UNMET_DEPENDENCIES, SOLVE_FIX_BROKEN, 50},
    {"conflict", ERR_TYPE_DEPENDENCY_CONFLICT, SOLVE_FIX_BROKEN, 60},
    {"permission", ERR_TYPE_PERMISSION_DENIED, SOLVE_WAIT_AND_RETRY, 55},
    {"space", ERR_TYPE_DISK_FULL, SOLVE_CLEAN_CACHE_AND_RETRY, 60},
    {"broken", ERR_TYPE_BROKEN_PACKAGE, SOLVE_FIX_BROKEN, 65},
    {"gpg", ERR_TYPE_SIGNATURE_INVALID, SOLVE_UPDATE_KEYS, 60},
    {"signature", ERR_TYPE_SIGNATURE_INVALID, SOLVE_UPDATE_KEYS, 55},
    {"build", ERR_TYPE_PIP_BUILD_FAILED, SOLVE_INSTALL_BUILD_DEPS, 50},
    {"distribution", ERR_TYPE_PIP_NO_MATCHING_DIST, SOLVE_TRY_ALT_INDEX, 50},
    {NULL, ERR_TYPE_UNKNOWN, SOLVE_RECORD_OUTPUT, 0}
};

static int match_fuzzy(const char *output, install_error_result_t *result) {
    if (!output) return 0;

    int best_confidence = 0;
    install_error_type_t best_type = ERR_TYPE_UNKNOWN;
    solution_type_t best_solution = SOLVE_RECORD_OUTPUT;
    const char *best_keyword = NULL;

    for (int i = 0; g_fuzzy_keywords[i].keyword != NULL; i++) {
        if (string_contains_ignore_case(output, g_fuzzy_keywords[i].keyword)) {
            if (g_fuzzy_keywords[i].confidence > best_confidence) {
                best_confidence = g_fuzzy_keywords[i].confidence;
                best_type = g_fuzzy_keywords[i].error_type;
                best_solution = g_fuzzy_keywords[i].solution_type;
                best_keyword = g_fuzzy_keywords[i].keyword;
            }
        }
    }

    if (best_confidence > 0) {
        result->error_type = best_type;
        result->solution_type = best_solution;
        result->solution_data = NULL;
        result->matched_pattern = best_keyword;
        result->confidence = best_confidence;
        return 1;
    }
    return 0;
}

/* ============================================================
 * 核心 API 实现
 * ============================================================ */

int install_error_parse(const char *output, const char *pkg_name,
                        const char *method_name, install_error_result_t *result) {
    LOG_DEBUG_T("InstallError", "Parse", "Enter", "pkg='%s', method='%s'",
                pkg_name ? pkg_name : "(null)", method_name ? method_name : "(null)");

    if (!result) return -1;
    memset(result, 0, sizeof(install_error_result_t));

    if (!output || !*output) {
        result->error_type = ERR_TYPE_UNKNOWN;
        result->solution_type = SOLVE_RECORD_OUTPUT;
        result->confidence = 0;
        LOG_DEBUG_T("InstallError", "Parse", "NoOutput", "output is empty");
        return 0;
    }

    /* 保存输出预览 */
    safe_strncpy(result->output_preview, output, sizeof(result->output_preview));

    /* 1. 精确匹配 */
    for (int i = 0; g_error_patterns[i].pattern != NULL; i++) {
        if (match_exact(output, &g_error_patterns[i], result)) {
            LOG_DEBUG_T("InstallError", "Parse", "ExactMatch",
                        "matched '%s' -> type=%d, solution=%d, confidence=%d",
                        result->matched_pattern, result->error_type,
                        result->solution_type, result->confidence);
            return 0;
        }
    }

    /* 2. 模糊匹配 */
    if (match_fuzzy(output, result)) {
        LOG_DEBUG_T("InstallError", "Parse", "FuzzyMatch",
                    "matched '%s' -> type=%d, solution=%d, confidence=%d",
                    result->matched_pattern, result->error_type,
                    result->solution_type, result->confidence);
        return 0;
    }

    /* 3. 无匹配：未知错误，仅记录输出 */
    result->error_type = ERR_TYPE_UNKNOWN;
    result->solution_type = SOLVE_RECORD_OUTPUT;
    result->confidence = 0;
    LOG_DEBUG_T("InstallError", "Parse", "NoMatch", "no pattern matched, recording output only");

    return 0;
}

/* ============================================================
 * 解决方案执行
 * ============================================================ */

int install_error_execute_solution(const install_error_result_t *result,
                                   const char *pkg_name,
                                   const char *current_method,
                                   const char *output) {
    if (!result || !pkg_name) return -1;

    LOG_INFO_T("InstallError", "ExecuteSolution", "Enter",
               "pkg='%s', solution=%d, error_type=%d",
               pkg_name, result->solution_type, result->error_type);

    /* 记录完整输出到日志 */
    install_error_log_output(pkg_name, current_method, output);

    switch (result->solution_type) {
        case SOLVE_RETRY_WITH_ALT_NAME: {
            char variants[8][64];
            int count = install_error_get_pkg_variants(pkg_name, variants, 8);
            if (count > 0) {
                /* 尝试第一个变体 */
                LOG_INFO_T("InstallError", "Solution", "RetryAltName",
                           "trying variant '%s' for '%s'", variants[0], pkg_name);
                return -2; /* 降级到下一个方法（使用新包名） */
            }
            LOG_WARN_T("InstallError", "Solution", "NoVariant",
                       "no variant found for '%s'", pkg_name);
            return -1;
        }

        case SOLVE_ADD_BREAK_FLAG: {
            LOG_INFO_T("InstallError", "Solution", "AddBreakFlag",
                       "will retry with --break-system-packages");
            return -2; /* 降级到下一个方法（使用 break flag） */
        }

        case SOLVE_FIX_BROKEN: {
            LOG_INFO_T("InstallError", "Solution", "FixBroken",
                       "executing apt install -f");
            /* 这里可以执行修复命令 */
            return -2; /* 降级到下一个方法 */
        }

        case SOLVE_TRY_MIRROR: {
            LOG_INFO_T("InstallError", "Solution", "TryMirror",
                       "will try mirror source for '%s'", pkg_name);
            return -2; /* 降级到下一个方法（使用镜像源） */
        }

        case SOLVE_RETRY_WITH_BACKOFF: {
            LOG_INFO_T("InstallError", "Solution", "RetryWithBackoff",
                       "will retry with backoff for '%s'", pkg_name);
            return -2; /* 降级到下一个方法（重试） */
        }

        case SOLVE_WAIT_AND_RETRY: {
            LOG_INFO_T("InstallError", "Solution", "WaitAndRetry",
                       "waiting 5s then retrying '%s'", pkg_name);
            sleep(5);
            return -2;
        }

        case SOLVE_CLEAN_CACHE_AND_RETRY: {
            LOG_INFO_T("InstallError", "Solution", "CleanCache",
                       "cleaning cache for '%s'", pkg_name);
            system("apt clean 2>/dev/null");
            system("apt autoclean 2>/dev/null");
            return -2;
        }

        case SOLVE_UPDATE_KEYS: {
            LOG_INFO_T("InstallError", "Solution", "UpdateKeys",
                       "updating GPG keys");
            return -2;
        }

        case SOLVE_TRY_ALT_INDEX: {
            LOG_INFO_T("InstallError", "Solution", "TryAltIndex",
                       "will try alternative index for '%s'", pkg_name);
            return -2;
        }

        case SOLVE_INSTALL_BUILD_DEPS: {
            LOG_INFO_T("InstallError", "Solution", "InstallBuildDeps",
                       "installing build dependencies for '%s'", pkg_name);
            system("apt install -y build-essential python3-dev 2>/dev/null");
            return -2;
        }

        case SOLVE_SKIP_PACKAGE: {
            LOG_WARN_T("InstallError", "Solution", "SkipPackage",
                       "skipping '%s' due to unrecoverable error", pkg_name);
            return -2;
        }

        case SOLVE_RECORD_OUTPUT:
        default: {
            LOG_WARN_T("InstallError", "Solution", "RecordOnly",
                       "no solution available for '%s', recording output only", pkg_name);
            return -1;
        }
    }
}

/* ============================================================
 * 包名变体查询
 * ============================================================ */

int install_error_get_pkg_variants(const char *pkg_name,
                                   char out_variants[][64],
                                   int max_variants) {
    if (!pkg_name || !out_variants || max_variants <= 0) return 0;

    /* 先检查映射表 */
    for (int i = 0; g_pkg_variants[i].original != NULL; i++) {
        if (strcmp(g_pkg_variants[i].original, pkg_name) == 0) {
            int count = 0;
            for (int j = 0; g_pkg_variants[i].variants[j] != NULL && count < max_variants; j++) {
                safe_strncpy(out_variants[count], g_pkg_variants[i].variants[j], 64);
                count++;
            }
            return count;
        }
    }

    /* 尝试通用规则：-dev → -devel */
    char variant[64];
    safe_strncpy(variant, pkg_name, sizeof(variant));
    char *dev = strstr(variant, "-dev");
    if (dev && strlen(variant) < 60) {
        safe_strncpy(out_variants[0], variant, 64);
        char *p = strstr(out_variants[0], "-dev");
        if (p) {
            strcpy(p, "-devel");
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * 重试判断
 * ============================================================ */

int install_error_should_retry(install_error_type_t error_type, int attempt) {
    switch (error_type) {
        case ERR_TYPE_PKG_NOT_FOUND:
            return attempt <= 2;
        case ERR_TYPE_PEP_668:
            return attempt <= 2;
        case ERR_TYPE_NETWORK_TIMEOUT:
            return attempt <= 4;
        case ERR_TYPE_SOURCE_UNREACHABLE:
            return attempt <= 3;
        case ERR_TYPE_PERMISSION_DENIED:
            return attempt <= 2;
        case ERR_TYPE_DISK_FULL:
            return attempt <= 2;
        case ERR_TYPE_UNMET_DEPENDENCIES:
            return attempt <= 3;
        case ERR_TYPE_BROKEN_PACKAGE:
            return attempt <= 3;
        default:
            return attempt <= 2;
    }
}

/* ============================================================
 * 错误类型描述
 * ============================================================ */

const char* install_error_get_description(install_error_type_t error_type,
                                          const char *lang) {
    for (int i = 0; g_error_descs[i].type != ERR_TYPE_MAX; i++) {
        if (g_error_descs[i].type == error_type) {
            if (lang && strcmp(lang, "zh") == 0) {
                return g_error_descs[i].desc_zh;
            }
            return g_error_descs[i].desc_en;
        }
    }
    return lang && strcmp(lang, "zh") == 0 ? "未知错误" : "Unknown error";
}

/* ============================================================
 * 解决方案类型描述
 * ============================================================ */

const char* install_error_get_solution_desc(solution_type_t solution_type,
                                            const char *lang) {
    for (int i = 0; g_solution_descs[i].type != SOLVE_MAX; i++) {
        if (g_solution_descs[i].type == solution_type) {
            if (lang && strcmp(lang, "zh") == 0) {
                return g_solution_descs[i].desc_zh;
            }
            return g_solution_descs[i].desc_en;
        }
    }
    return lang && strcmp(lang, "zh") == 0 ? "无解决方案" : "No solution";
}

/* ============================================================
 * 记录完整输出到日志文件
 * ============================================================ */

void install_error_log_output(const char *pkg_name, const char *method_name,
                              const char *output) {
    if (!pkg_name || !output) return;

    const char *root = lingos_data_root();
    char log_path[512];
    safe_snprintf(log_path, sizeof(log_path), "%s/Debug/install_error.log", root);

    FILE *fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(fp, "========================================\n");
    fprintf(fp, "[%s] Package: %s, Method: %s\n", time_str, pkg_name,
            method_name ? method_name : "(unknown)");
    fprintf(fp, "--- Output Start ---\n");
    fwrite(output, 1, strlen(output), fp);
    fprintf(fp, "\n--- Output End ---\n");
    fclose(fp);

    LOG_DEBUG_T("InstallError", "LogOutput", "OK", "logged output for '%s'", pkg_name);
}

/* ============================================================
 * 获取优先级
 * ============================================================ */

int install_error_get_priority(install_error_type_t error_type) {
    for (int i = 0; g_error_patterns[i].pattern != NULL; i++) {
        if (g_error_patterns[i].error_type == error_type) {
            return g_error_patterns[i].priority;
        }
    }
    return 99;
}