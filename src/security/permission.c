/**
 * @file    permission.c
 * @brief   基于令牌的细粒度权限管理系统（自动创建默认配置）
 * @version 2.1.0.0
 */

#include "permission.h"
#include "log_extra.h"
#include "data_path.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define TOKEN_LEN 64
#define MAX_TOKENS 256
#define PERM_STR_LEN 128

typedef struct {
    char token[TOKEN_LEN];
    char role[32];
    uint64_t expire;
    char permissions[10][PERM_STR_LEN];
    int perm_count;
    char task_id[64];
} token_entry_t;

static token_entry_t token_table[MAX_TOKENS];
static int token_count = 0;

static const char *get_roles_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s/system/config/roles.json", root);
    }
    return path;
}

static const char *get_tokens_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s/Ensystem/tokens.json", root);
    }
    return path;
}

/* 创建默认 roles.json（如果不存在） */
static void create_default_roles(void) {
    const char *path = get_roles_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Permission", "CreateRoles", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp,
        "{\n"
        "  \"roles\": {\n"
        "    \"main_ai\": {\n"
        "      \"permissions\": [\"*\"],\n"
        "      \"risk_ack_required\": [\"skill:execute:system_reboot\", \"skill:execute:file_delete:/\"]\n"
        "    },\n"
        "    \"sub_ai_default\": {\n"
        "      \"permissions\": [\"skill:execute:file_read\", \"skill:execute:file_list\", \"skill:execute:process_list\"],\n"
        "      \"max_cpu_percent\": 50,\n"
        "      \"max_memory_mb\": 256\n"
        "    },\n"
        "    \"system_service\": {\n"
        "      \"permissions\": [\"skill:execute:*\"],\n"
        "      \"max_cpu_percent\": 80,\n"
        "      \"max_memory_mb\": 512\n"
        "    }\n"
        "  }\n"
        "}\n");
    fclose(fp);
    LOG_INFO_T("Permission", "CreateRoles", "OK", "created %s", path);
}

/* 创建空 tokens.json */
static void create_empty_tokens(void) {
    const char *path = get_tokens_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Permission", "CreateTokens", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp, "{\"tokens\": []}\n");
    fclose(fp);
    LOG_INFO_T("Permission", "CreateTokens", "OK", "created %s", path);
}

void permission_init(void) {
    memset(token_table, 0, sizeof(token_table));
    token_count = 0;
    create_default_roles();
    create_empty_tokens();
    LOG_INFO_T("Permission", "Init", "OK", "Token-based permission system initialized");
}

static int generate_random_token(char *out, size_t len) {
    const char *alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    FILE *urandom = fopen("/dev/urandom", "r");
    if (!urandom) {
        srand(time(NULL));
        for (size_t i = 0; i < len-1; i++) {
            out[i] = alphabet[rand() % (sizeof(alphabet)-1)];
        }
        out[len-1] = '\0';
        return 0;
    }
    unsigned char rand_buf[TOKEN_LEN];
    fread(rand_buf, 1, TOKEN_LEN, urandom);
    fclose(urandom);
    for (size_t i = 0; i < len-1 && i < TOKEN_LEN; i++) {
        out[i] = alphabet[rand_buf[i] % (sizeof(alphabet)-1)];
    }
    out[len-1] = '\0';
    return 0;
}

static int check_permission_string(const char *perm_str, const char *required) {
    if (strcmp(perm_str, "*") == 0) return 1;
    if (strcmp(perm_str, required) == 0) return 1;
    if (strstr(perm_str, "*")) {
        const char *star = strchr(perm_str, '*');
        size_t prefix_len = star - perm_str;
        if (strncmp(perm_str, required, prefix_len) == 0) return 1;
    }
    return 0;
}

int permission_check_token(const char *token, const char *required_perm) {
    if (!token || !required_perm) return 0;
    for (int i = 0; i < token_count; i++) {
        if (strcmp(token_table[i].token, token) == 0) {
            if (token_table[i].expire > 0 && token_table[i].expire < (uint64_t)time(NULL)) {
                LOG_WARN_T("Permission", "Check", "Expired", "token %s expired", token);
                return 0;
            }
            for (int p = 0; p < token_table[i].perm_count; p++) {
                if (check_permission_string(token_table[i].permissions[p], required_perm)) {
                    return 1;
                }
            }
            return 0;
        }
    }
    return 0;
}

int permission_grant_token(const char *role, const char **perms, int perm_count,
                           int expire_seconds, char *out_token, size_t out_len) {
    if (token_count >= MAX_TOKENS) return -1;
    char token[TOKEN_LEN];
    if (generate_random_token(token, sizeof(token)) != 0) return -1;
    token_entry_t *entry = &token_table[token_count];
    strncpy(entry->token, token, sizeof(entry->token)-1);
    strncpy(entry->role, role, sizeof(entry->role)-1);
    entry->expire = (expire_seconds > 0) ? (uint64_t)time(NULL) + expire_seconds : 0;
    entry->perm_count = perm_count;
    for (int i = 0; i < perm_count && i < 10; i++) {
        strncpy(entry->permissions[i], perms[i], PERM_STR_LEN-1);
    }
    entry->task_id[0] = '\0';
    token_count++;
    if (out_token) strncpy(out_token, token, out_len-1);
    LOG_INFO_T("Permission", "Grant", "OK", "token=%s role=%s expire=%d", token, role, expire_seconds);
    return 0;
}

int permission_revoke_token(const char *token) {
    for (int i = 0; i < token_count; i++) {
        if (strcmp(token_table[i].token, token) == 0) {
            if (i < token_count - 1) {
                token_table[i] = token_table[token_count - 1];
            }
            token_count--;
            LOG_INFO_T("Permission", "Revoke", "OK", "token=%s", token);
            return 0;
        }
    }
    return -1;
}

void permission_set_token_task_id(const char *token, const char *task_id) {
    for (int i = 0; i < token_count; i++) {
        if (strcmp(token_table[i].token, token) == 0) {
            strncpy(token_table[i].task_id, task_id, sizeof(token_table[i].task_id)-1);
            break;
        }
    }
}

/* 旧接口实现（恒返回允许，因为新权限系统已接管） */
int permission_check(uint32_t app_id, permission_t perm) {
    (void)app_id; (void)perm;
    return 1;
}

auth_mode_t permission_request(uint32_t app_id, permission_t perm) {
    (void)app_id; (void)perm;
    return AUTH_MODE_ALLOW_ALWAYS;
}

void permission_set_mode(uint32_t app_id, permission_t perm, auth_mode_t mode) {
    (void)app_id; (void)perm; (void)mode;
}

void permission_set_bg_mode(uint32_t app_id, bg_mode_t mode, uint32_t minutes) {
    (void)app_id; (void)mode; (void)minutes;
}

void permission_reload_config(void) {
    LOG_INFO_T("Permission", "Reload", "OK", "reload not implemented (tokens in memory only)");
}

uint32_t permission_get_bg_remaining(uint32_t app_id) {
    (void)app_id;
    return 0;
}

const char* permission_get_name(permission_t p) {
    return "unknown";
}

const char* auth_mode_get_name(auth_mode_t m) {
    return "unknown";
}