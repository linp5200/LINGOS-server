/**
 * @file    src/ai/ai_privilege.c
 * @brief   AI 权限管理核心实现（L0-L3 分级 + 自动授权 + 临时权限）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C3, C-C, AI-CTL
 * @changes 修复头文件引用与并发问题：
 *          - 【新增】#include <pthread.h>（此前 pthread_mutex_t 未声明导致编译失败）
 *          - 【C3 修正】相对路径 include 改为文件名直接引用（需 Makefile -I 配合）
 *          - 【修复】ai_privilege_reload() 双重加锁死锁（load_config 内部自行加锁）
 *          - 【修复】ai_privilege_get_status() malloc 与字面量混合返回 -> static 缓冲
 */

#include "ai_privilege.h"
#include "safe_string.h"
#include "data_path.h"
#include "log_extra.h"
#include "config_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#define MAX_RULES 64
#define MAX_SKILL_NAME 64
#define MAX_CACHE_ENTRIES 32

/* ============================================================
 * 权限缓存条目
 * ============================================================ */
typedef struct {
    char skill[MAX_SKILL_NAME];
    int level;
    time_t granted_at;
    time_t expires_at;
    int active;
} privilege_cache_entry_t;

/* ============================================================
 * 权限规则（从配置文件加载）
 * ============================================================ */
typedef struct {
    char skill[MAX_SKILL_NAME];
    char action[16];           /* "auto", "confirm", "block", 或秒数 */
    int duration_sec;          /* 自动授权的持续时间（秒），0 表示永久 */
    int enabled;
} privilege_rule_t;

/* ============================================================
 * 全局状态
 * ============================================================ */
static privilege_cache_entry_t g_cache[MAX_CACHE_ENTRIES];
static int g_cache_count = 0;
static privilege_rule_t g_rules[MAX_RULES];
static int g_rule_count = 0;
static int g_privilege_initialized = 0;
static time_t g_last_reload = 0;
static pthread_mutex_t g_privilege_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助
 * ============================================================ */
static void trim_whitespace(char *str) {
    if (!str) return;
    char *end;
    while (*str == ' ' || *str == '\t') str++;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
}

/* ============================================================
 * FTF[获取配置文件路径]
 * ============================================================ */
static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s/system/config/ai_privilege.conf", root);
    }
    return path;
}

/* ============================================================
 * FTF[创建默认配置文件]
 * ============================================================ */
static void create_default_config(void) {
    const char *path = get_config_path();
    if (access(path, F_OK) == 0) return;

    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("AI Privilege", "CreateConfig", "Fail", "cannot create %s", path);
        return;
    }

    fprintf(fp,
        "# LING OS AI Privilege Configuration\n"
        "# Format: skill_name = auto|confirm|block|seconds\n"
        "#\n"
        "# L0 (Read-Only) - Always auto-authorized\n"
        "file_read = auto\n"
        "file_list = auto\n"
        "system_info = auto\n"
        "system_memory = auto\n"
        "system_disk = auto\n"
        "system_cpu = auto\n"
        "net_ping = auto\n"
        "net_status = auto\n"
        "process_list = auto\n"
        "memory_read = auto\n"
        "memory_search = auto\n"
        "\n"
        "# L1 (Regular) - Auto-authorized with time limit\n"
        "file_write = 300\n"
        "file_copy = 300\n"
        "file_move = 300\n"
        "file_mkdir = 300\n"
        "package_list = auto\n"
        "service_status = auto\n"
        "config_read = auto\n"
        "\n"
        "# L2 (High Impact) - Confirm by default\n"
        "file_delete = confirm\n"
        "service_restart = confirm\n"
        "package_install = confirm\n"
        "\n"
        "# L3 (Dangerous) - Must confirm\n"
        "sys_command = confirm\n"
        "package_remove = confirm\n"
        "system_update = confirm\n"
    );
    fclose(fp);
    LOG_INFO_T("AI Privilege", "CreateConfig", "OK", "created %s", path);
}

/* ============================================================
 * FTF[加载配置文件]（内部自行加锁）
 * ============================================================ */
static int load_config(void) {
    LOG_DEBUG_T("AI Privilege", "LoadConfig", "Enter", "loading privilege config");

    pthread_mutex_lock(&g_privilege_lock);

    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        create_default_config();
        fp = fopen(path, "r");
        if (!fp) {
            LOG_WARN_T("AI Privilege", "LoadConfig", "Fail", "cannot open %s", path);
            pthread_mutex_unlock(&g_privilege_lock);
            return -1;
        }
    }

    g_rule_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && g_rule_count < MAX_RULES) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char skill[MAX_SKILL_NAME];
        char value[32];
        if (sscanf(line, "%63[^=]=%31s", skill, value) != 2) continue;

        trim_whitespace(skill);
        trim_whitespace(value);

        privilege_rule_t *rule = &g_rules[g_rule_count];
        safe_strncpy(rule->skill, skill, sizeof(rule->skill));
        safe_strncpy(rule->action, value, sizeof(rule->action));

        if (strcmp(value, "auto") == 0) {
            rule->duration_sec = 0;   /* 永久 */
            rule->enabled = 1;
        } else if (strcmp(value, "confirm") == 0) {
            rule->duration_sec = 0;
            rule->enabled = 1;
        } else if (strcmp(value, "block") == 0) {
            rule->duration_sec = 0;
            rule->enabled = 0;
        } else {
            /* 尝试解析为秒数 */
            int sec = atoi(value);
            if (sec > 0) {
                rule->duration_sec = sec;
                rule->enabled = 1;
                safe_strncpy(rule->action, "auto", sizeof(rule->action));
            } else {
                /* 无效规则，禁用 */
                rule->enabled = 0;
                safe_strncpy(rule->action, "block", sizeof(rule->action));
            }
        }
        g_rule_count++;
    }
    fclose(fp);

    g_last_reload = time(NULL);

    LOG_INFO_T("AI Privilege", "LoadConfig", "OK", "loaded %d rules", g_rule_count);
    pthread_mutex_unlock(&g_privilege_lock);
    return 0;
}

/* ============================================================
 * FTF[查找权限规则]
 * ============================================================ */
static privilege_rule_t* find_rule(const char *skill_name) {
    if (!skill_name) return NULL;
    for (int i = 0; i < g_rule_count; i++) {
        if (strcmp(g_rules[i].skill, skill_name) == 0 && g_rules[i].enabled) {
            return &g_rules[i];
        }
    }
    return NULL;
}

/* ============================================================
 * FTF[查找缓存条目]
 * ============================================================ */
static privilege_cache_entry_t* find_cache(const char *skill_name) {
    if (!skill_name) return NULL;
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i].skill, skill_name) == 0 && g_cache[i].active) {
            return &g_cache[i];
        }
    }
    return NULL;
}

/* ============================================================
 * FTF[清理过期缓存]
 * ============================================================ */
static void clean_expired_cache(void) {
    time_t now = time(NULL);
    for (int i = 0; i < g_cache_count; i++) {
        if (g_cache[i].active && g_cache[i].expires_at > 0) {
            if (now >= g_cache[i].expires_at) {
                g_cache[i].active = 0;
                LOG_DEBUG_T("AI Privilege", "Cache", "Expired", "skill '%s' expired", g_cache[i].skill);
            }
        }
    }
}

/* ============================================================
 * FTF[添加缓存条目]
 * ============================================================ */
static int add_cache(const char *skill_name, int level, int duration_sec) {
    if (!skill_name) return -1;

    clean_expired_cache();

    /* 先尝试更新现有条目 */
    privilege_cache_entry_t *existing = find_cache(skill_name);
    if (existing) {
        existing->level = level;
        existing->granted_at = time(NULL);
        existing->expires_at = (duration_sec > 0) ? time(NULL) + duration_sec : 0;
        existing->active = 1;
        LOG_DEBUG_T("AI Privilege", "Cache", "Update", "skill '%s' updated, level=%d", skill_name, level);
        return 0;
    }

    if (g_cache_count >= MAX_CACHE_ENTRIES) {
        /* 移除最旧的过期条目 */
        for (int i = 0; i < g_cache_count; i++) {
            if (!g_cache[i].active) {
                memmove(&g_cache[i], &g_cache[i + 1], sizeof(privilege_cache_entry_t) * (g_cache_count - i - 1));
                g_cache_count--;
                break;
            }
        }
        if (g_cache_count >= MAX_CACHE_ENTRIES) {
            LOG_WARN_T("AI Privilege", "Cache", "Full", "cache full, cannot add '%s'", skill_name);
            return -1;
        }
    }

    privilege_cache_entry_t *entry = &g_cache[g_cache_count++];
    safe_strncpy(entry->skill, skill_name, sizeof(entry->skill));
    entry->level = level;
    entry->granted_at = time(NULL);
    entry->expires_at = (duration_sec > 0) ? time(NULL) + duration_sec : 0;
    entry->active = 1;

    LOG_DEBUG_T("AI Privilege", "Cache", "Add", "skill '%s' added, level=%d, expires=%ld",
                skill_name, level, entry->expires_at);
    return 0;
}

/* ============================================================
 * FTF[根据技能获取权限等级]（调用方须持有锁）
 * ============================================================ */
static int get_skill_level_internal(const char *skill_name) {
    if (!skill_name) return PRIVILEGE_LEVEL_L0;

    /* 先查缓存 */
    privilege_cache_entry_t *cache = find_cache(skill_name);
    if (cache && cache->active) {
        LOG_DEBUG_T("AI Privilege", "GetLevel", "CacheHit", "skill '%s' level=%d", skill_name, cache->level);
        return cache->level;
    }

    /* 查规则 */
    privilege_rule_t *rule = find_rule(skill_name);
    if (rule) {
        if (strcmp(rule->action, "auto") == 0) {
            /* 自动授权 */
            int level = PRIVILEGE_LEVEL_L0;
            if (rule->duration_sec > 0) level = PRIVILEGE_LEVEL_L1;
            add_cache(skill_name, level, rule->duration_sec);
            LOG_DEBUG_T("AI Privilege", "GetLevel", "AutoGrant", "skill '%s' auto-granted level=%d", skill_name, level);
            return level;
        }
        if (strcmp(rule->action, "confirm") == 0) {
            LOG_DEBUG_T("AI Privilege", "GetLevel", "NeedConfirm", "skill '%s' needs confirmation", skill_name);
            return PRIVILEGE_LEVEL_L2;
        }
        if (strcmp(rule->action, "block") == 0) {
            LOG_DEBUG_T("AI Privilege", "GetLevel", "Blocked", "skill '%s' is blocked", skill_name);
            return PRIVILEGE_LEVEL_L3;  /* 实际权限检查会拒绝 */
        }
    }

    /* 默认：未知技能按 L0 处理 */
    LOG_DEBUG_T("AI Privilege", "GetLevel", "Default", "skill '%s' using default L0", skill_name);
    return PRIVILEGE_LEVEL_L0;
}

/* ============================================================
 * FTF[请求授权（AI 调用）]
 * ============================================================ */
int ai_privilege_request(const char *skill_name, const char *reason, int *out_level) {
    LOG_DEBUG_T("AI Privilege", "Request", "Enter", "skill='%s', reason='%s'",
                skill_name ? skill_name : "(null)", reason ? reason : "(null)");

    if (!skill_name) return PRIVILEGE_RESULT_ERROR;

    pthread_mutex_lock(&g_privilege_lock);

    int level = get_skill_level_internal(skill_name);

    /* 检查缓存中是否已有授权 */
    privilege_cache_entry_t *cache = find_cache(skill_name);
    if (cache && cache->active) {
        if (cache->level >= level) {
            *out_level = cache->level;
            pthread_mutex_unlock(&g_privilege_lock);
            LOG_INFO_T("AI Privilege", "Request", "Cached", "skill='%s' cached approval", skill_name);
            return PRIVILEGE_RESULT_GRANTED;
        }
    }

    /* 检查规则 */
    privilege_rule_t *rule = find_rule(skill_name);
    if (rule) {
        if (strcmp(rule->action, "auto") == 0) {
            /* 自动授权 */
            int granted_level = (rule->duration_sec > 0) ? PRIVILEGE_LEVEL_L1 : PRIVILEGE_LEVEL_L0;
            add_cache(skill_name, granted_level, rule->duration_sec);
            *out_level = granted_level;
            pthread_mutex_unlock(&g_privilege_lock);
            LOG_INFO_T("AI Privilege", "Request", "AutoGrant", "skill='%s' auto-granted", skill_name);
            return PRIVILEGE_RESULT_GRANTED;
        }
        if (strcmp(rule->action, "block") == 0) {
            pthread_mutex_unlock(&g_privilege_lock);
            LOG_WARN_T("AI Privilege", "Request", "Blocked", "skill='%s' is blocked", skill_name);
            *out_level = PRIVILEGE_LEVEL_L3;
            return PRIVILEGE_RESULT_DENIED;
        }
    }

    /* 需要确认或规则不存在 → 返回待确认状态 */
    pthread_mutex_unlock(&g_privilege_lock);
    LOG_INFO_T("AI Privilege", "Request", "NeedAuth", "skill='%s' needs authorization", skill_name);
    *out_level = PRIVILEGE_LEVEL_L2;
    return PRIVILEGE_RESULT_NEED_AUTH;
}

/* ============================================================
 * FTF[授予权限（用户确认后）]
 * ============================================================ */
int ai_privilege_grant(const char *skill_name, int level, int duration_sec) {
    LOG_INFO_T("AI Privilege", "Grant", "Enter", "skill='%s', level=%d, duration=%d",
               skill_name ? skill_name : "(null)", level, duration_sec);

    if (!skill_name) return -1;

    pthread_mutex_lock(&g_privilege_lock);

    int ret = add_cache(skill_name, level, duration_sec);

    pthread_mutex_unlock(&g_privilege_lock);

    if (ret == 0) {
        LOG_INFO_T("AI Privilege", "Grant", "OK", "skill='%s' granted level=%d", skill_name, level);
    } else {
        LOG_ERROR_T("AI Privilege", "Grant", "Fail", "failed to grant skill='%s'", skill_name);
    }
    return ret;
}

/* ============================================================
 * FTF[撤销权限]
 * ============================================================ */
int ai_privilege_revoke(const char *skill_name) {
    LOG_INFO_T("AI Privilege", "Revoke", "Enter", "skill='%s'", skill_name ? skill_name : "(null)");

    if (!skill_name) return -1;

    pthread_mutex_lock(&g_privilege_lock);

    privilege_cache_entry_t *cache = find_cache(skill_name);
    if (cache) {
        cache->active = 0;
        pthread_mutex_unlock(&g_privilege_lock);
        LOG_INFO_T("AI Privilege", "Revoke", "OK", "skill='%s' revoked", skill_name);
        return 0;
    }

    pthread_mutex_unlock(&g_privilege_lock);
    LOG_WARN_T("AI Privilege", "Revoke", "NotFound", "skill='%s' not in cache", skill_name);
    return -1;
}

/* ============================================================
 * FTF[检查权限（核心检查函数）]
 * ============================================================ */
int ai_privilege_check(const char *skill_name) {
    if (!skill_name) return 0;

    pthread_mutex_lock(&g_privilege_lock);

    /* 检查缓存 */
    privilege_cache_entry_t *cache = find_cache(skill_name);
    if (cache && cache->active) {
        pthread_mutex_unlock(&g_privilege_lock);
        LOG_DEBUG_T("AI Privilege", "Check", "OK", "skill='%s' approved (level=%d)", skill_name, cache->level);
        return 1;
    }

    pthread_mutex_unlock(&g_privilege_lock);
    LOG_DEBUG_T("AI Privilege", "Check", "Denied", "skill='%s' not approved", skill_name);
    return 0;
}

/* ============================================================
 * FTF[获取技能所需权限等级]
 * ============================================================ */
int ai_privilege_get_level(const char *skill_name) {
    if (!skill_name) return PRIVILEGE_LEVEL_L0;

    pthread_mutex_lock(&g_privilege_lock);
    int level = get_skill_level_internal(skill_name);
    pthread_mutex_unlock(&g_privilege_lock);

    LOG_DEBUG_T("AI Privilege", "GetLevel", "Result", "skill='%s' level=%d", skill_name, level);
    return level;
}

/* ============================================================
 * FTF[检查技能是否为高风险（L2/L3）]
 * ============================================================ */
int ai_privilege_is_high_risk(const char *skill_name) {
    int level = ai_privilege_get_level(skill_name);
    return (level >= PRIVILEGE_LEVEL_L2);
}

/* ============================================================
 * FTF[获取权限状态字符串]（static 缓冲，线程安全）
 * ============================================================ */
const char* ai_privilege_get_status(const char *skill_name) {
    static char status_buf[64];

    if (!skill_name) return "unknown";

    pthread_mutex_lock(&g_privilege_lock);

    privilege_cache_entry_t *cache = find_cache(skill_name);
    if (cache && cache->active) {
        if (cache->expires_at > 0) {
            long remaining = cache->expires_at - time(NULL);
            if (remaining > 0) {
                safe_snprintf(status_buf, sizeof(status_buf), "granted (expires in %lds)", remaining);
            } else {
                safe_strncpy(status_buf, "expired", sizeof(status_buf));
                cache->active = 0;
            }
        } else {
            safe_strncpy(status_buf, "granted (permanent)", sizeof(status_buf));
        }
        pthread_mutex_unlock(&g_privilege_lock);
        return status_buf;
    }

    /* 检查规则 */
    privilege_rule_t *rule = find_rule(skill_name);
    if (rule) {
        if (strcmp(rule->action, "auto") == 0) {
            pthread_mutex_unlock(&g_privilege_lock);
            return rule->duration_sec > 0 ? "auto (limited)" : "auto (permanent)";
        }
        if (strcmp(rule->action, "confirm") == 0) {
            pthread_mutex_unlock(&g_privilege_lock);
            return "requires confirmation";
        }
        if (strcmp(rule->action, "block") == 0) {
            pthread_mutex_unlock(&g_privilege_lock);
            return "blocked";
        }
    }

    pthread_mutex_unlock(&g_privilege_lock);
    return "unknown";
}

/* ============================================================
 * FTF[初始化 AI 权限系统]
 * ============================================================ */
int ai_privilege_init(void) {
    LOG_INFO_T("AI Privilege", "Init", "Enter", "initializing AI privilege system");

    if (g_privilege_initialized) {
        LOG_DEBUG_T("AI Privilege", "Init", "Already", "already initialized");
        return 0;
    }

    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;

    if (load_config() != 0) {
        LOG_WARN_T("AI Privilege", "Init", "ConfigFail", "using defaults");
    }

    g_privilege_initialized = 1;
    LOG_INFO_T("AI Privilege", "Init", "OK", "AI privilege system initialized with %d rules", g_rule_count);
    return 0;
}

/* ============================================================
 * FTF[重新加载配置]（修复双重加锁死锁）
 * ============================================================ */
int ai_privilege_reload(void) {
    LOG_INFO_T("AI Privilege", "Reload", "Enter", "reloading privilege config");

    /* 清空缓存（锁内完成，随后立即释放） */
    pthread_mutex_lock(&g_privilege_lock);
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
    pthread_mutex_unlock(&g_privilege_lock);

    /* load_config() 内部自行加锁，此处不再加锁，避免死锁 */
    int ret = load_config();

    if (ret == 0) {
        LOG_INFO_T("AI Privilege", "Reload", "OK", "reloaded successfully");
    } else {
        LOG_ERROR_T("AI Privilege", "Reload", "Fail", "reload failed");
    }
    return ret;
}

/* ============================================================
 * FTF[获取缓存统计]
 * ============================================================ */
void ai_privilege_get_stats(int *cache_count, int *rule_count) {
    pthread_mutex_lock(&g_privilege_lock);
    if (cache_count) *cache_count = g_cache_count;
    if (rule_count) *rule_count = g_rule_count;
    pthread_mutex_unlock(&g_privilege_lock);
}