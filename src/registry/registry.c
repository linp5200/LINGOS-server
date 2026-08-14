/**
 * @file    src/registry/registry.c
 * @brief   注册表核心实现（增加超时保护 + 锁修复）
 * @version LN-B-5.1.2.6-rc
 * @changes registry_load() 添加 3 秒超时保护；
 *          修复超时分支未释放锁导致死锁的问题；
 *          添加详细调试日志
 */

#include "registry.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>

#define REGISTRY_DIR "/registry/core"
#define INDEX_FILE "/registry/core/registry.json"
#define MAX_ENTRIES 1024
#define REGISTRY_LOAD_TIMEOUT 3  /* 超时秒数 */

static registry_entry_t g_entries[MAX_ENTRIES];
static int g_entry_count = 0;
static int g_initialized = 0;
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static registry_change_cb g_change_cb = NULL;
static void *g_change_user_data = NULL;

/* 超时跳转环境 */
static sigjmp_buf g_timeout_env;
static volatile int g_timeout_occurred = 0;

/* ============================================================
 * 内部辅助
 * ============================================================ */

static const char* get_index_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, INDEX_FILE);
    }
    return path;
}

static void ensure_registry_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/registry/core", root);
    if (access(dir, F_OK) != 0) {
        mkdir(dir, 0755);
    }
}

/* ============================================================
 * 超时信号处理器
 * ============================================================ */
static void registry_alarm_handler(int sig) {
    (void)sig;
    g_timeout_occurred = 1;
    LOG_DEBUG_T("Registry", "Alarm", "Timeout", "alarm triggered, about to siglongjmp");
    siglongjmp(g_timeout_env, 1);
}

/* ============================================================
 * 创建空注册表
 * ============================================================ */
void registry_create_empty(void) {
    LOG_INFO_T("Registry", "CreateEmpty", "Enter", "creating empty registry");
    LOG_DEBUG_T("Registry", "CreateEmpty", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);
    LOG_DEBUG_T("Registry", "CreateEmpty", "Lock", "registry lock acquired");
    g_entry_count = 0;
    memset(g_entries, 0, sizeof(g_entries));
    g_initialized = 1;
    LOG_DEBUG_T("Registry", "CreateEmpty", "Unlock", "releasing registry lock");
    pthread_mutex_unlock(&g_registry_lock);
    registry_save();
    LOG_INFO_T("Registry", "CreateEmpty", "OK", "empty registry created");
}

/* ============================================================
 * 核心 API 实现
 * ============================================================ */

int registry_init(void) {
    LOG_INFO_T("Registry", "Init", "Enter", "initializing registry");

    LOG_DEBUG_T("Registry", "Init", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    if (g_initialized) {
        LOG_DEBUG_T("Registry", "Init", "Unlock", "already initialized, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_DEBUG_T("Registry", "Init", "Already", "already initialized");
        return 0;
    }
    LOG_DEBUG_T("Registry", "Init", "Unlock", "releasing lock before directory check");
    pthread_mutex_unlock(&g_registry_lock);

    LOG_DEBUG_T("Registry", "Init", "Dir", "ensuring registry directory");
    ensure_registry_dir();

    LOG_DEBUG_T("Registry", "Init", "Load", "calling registry_load");
    int ret = registry_load();
    if (ret != 0) {
        LOG_WARN_T("Registry", "Init", "LoadFail", "registry load failed or timeout, creating empty registry");
        registry_create_empty();
        return 0;
    }

    LOG_INFO_T("Registry", "Init", "OK", "registry initialized with %d entries", g_entry_count);
    return 0;
}

int registry_register(const registry_entry_t *entry) {
    LOG_INFO_T("Registry", "Register", "Enter", "id='%s', type=%d", entry ? entry->id : "(null)", entry ? entry->type : -1);

    if (!entry || !entry->id[0]) {
        LOG_ERROR_T("Registry", "Register", "Invalid", "entry or id is NULL");
        return -1;
    }

    LOG_DEBUG_T("Registry", "Register", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    if (!g_initialized) {
        LOG_DEBUG_T("Registry", "Register", "Unlock", "not initialized, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Register", "NotInit", "registry not initialized");
        return -1;
    }

    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, entry->id) == 0) {
            g_entries[i] = *entry;
            g_entries[i].updated_at = time(NULL);
            LOG_DEBUG_T("Registry", "Register", "Unlock", "updating existing entry, releasing lock");
            pthread_mutex_unlock(&g_registry_lock);
            registry_save();
            if (g_change_cb) g_change_cb(entry->id, 1, g_change_user_data);
            LOG_INFO_T("Registry", "Register", "Updated", "updated existing entry '%s'", entry->id);
            return 0;
        }
    }

    if (g_entry_count >= MAX_ENTRIES) {
        LOG_DEBUG_T("Registry", "Register", "Unlock", "overflow, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Register", "Overflow", "max entries reached");
        return -1;
    }

    g_entries[g_entry_count] = *entry;
    g_entries[g_entry_count].created_at = time(NULL);
    g_entries[g_entry_count].updated_at = time(NULL);
    g_entry_count++;

    LOG_DEBUG_T("Registry", "Register", "Unlock", "releasing lock");
    pthread_mutex_unlock(&g_registry_lock);

    registry_save();
    if (g_change_cb) g_change_cb(entry->id, 0, g_change_user_data);

    LOG_INFO_T("Registry", "Register", "OK", "registered '%s' (total=%d)", entry->id, g_entry_count);
    return 0;
}

int registry_unregister(const char *id) {
    LOG_INFO_T("Registry", "Unregister", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("Registry", "Unregister", "Invalid", "id is NULL or empty");
        return -1;
    }

    LOG_DEBUG_T("Registry", "Unregister", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    int found = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) {
            found = 1;
            for (int j = i; j < g_entry_count - 1; j++) {
                g_entries[j] = g_entries[j + 1];
            }
            g_entry_count--;
            break;
        }
    }

    LOG_DEBUG_T("Registry", "Unregister", "Unlock", "releasing lock");
    pthread_mutex_unlock(&g_registry_lock);

    if (found) {
        registry_save();
        if (g_change_cb) g_change_cb(id, 2, g_change_user_data);
        LOG_INFO_T("Registry", "Unregister", "OK", "unregistered '%s'", id);
        return 0;
    }

    LOG_WARN_T("Registry", "Unregister", "NotFound", "id '%s' not found", id);
    return -1;
}

int registry_update(const char *id, const registry_entry_t *entry) {
    LOG_INFO_T("Registry", "Update", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id || !entry || !entry->id[0]) {
        LOG_ERROR_T("Registry", "Update", "Invalid", "invalid parameters");
        return -1;
    }

    LOG_DEBUG_T("Registry", "Update", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) {
            g_entries[i] = *entry;
            g_entries[i].updated_at = time(NULL);
            LOG_DEBUG_T("Registry", "Update", "Unlock", "releasing lock");
            pthread_mutex_unlock(&g_registry_lock);
            registry_save();
            if (g_change_cb) g_change_cb(id, 1, g_change_user_data);
            LOG_INFO_T("Registry", "Update", "OK", "updated '%s'", id);
            return 0;
        }
    }

    LOG_DEBUG_T("Registry", "Update", "Unlock", "not found, releasing lock");
    pthread_mutex_unlock(&g_registry_lock);
    LOG_WARN_T("Registry", "Update", "NotFound", "id '%s' not found", id);
    return -1;
}

const registry_entry_t* registry_get(const char *id) {
    if (!id || !*id) return NULL;

    LOG_DEBUG_T("Registry", "Get", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    for (int i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].id, id) == 0) {
            LOG_DEBUG_T("Registry", "Get", "Unlock", "found, releasing lock");
            pthread_mutex_unlock(&g_registry_lock);
            return &g_entries[i];
        }
    }

    LOG_DEBUG_T("Registry", "Get", "Unlock", "not found, releasing lock");
    pthread_mutex_unlock(&g_registry_lock);
    return NULL;
}

int registry_list(int type, registry_entry_t **out, int max_count) {
    if (!out || max_count <= 0) return 0;

    LOG_DEBUG_T("Registry", "List", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    int count = 0;
    for (int i = 0; i < g_entry_count && count < max_count; i++) {
        if (type < 0 || g_entries[i].type == (registry_type_t)type) {
            out[count++] = &g_entries[i];
        }
    }

    LOG_DEBUG_T("Registry", "List", "Unlock", "releasing lock");
    pthread_mutex_unlock(&g_registry_lock);
    LOG_DEBUG_T("Registry", "List", "OK", "returned %d entries", count);
    return count;
}

int registry_query(const char *query, registry_entry_t **out, int max_count) {
    if (!query || !out || max_count <= 0) return 0;

    LOG_DEBUG_T("Registry", "Query", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    int count = 0;
    for (int i = 0; i < g_entry_count && count < max_count; i++) {
        if (strstr(g_entries[i].id, query) || strstr(g_entries[i].name, query)) {
            out[count++] = &g_entries[i];
        }
    }

    LOG_DEBUG_T("Registry", "Query", "Unlock", "releasing lock");
    pthread_mutex_unlock(&g_registry_lock);
    LOG_DEBUG_T("Registry", "Query", "OK", "found %d entries matching '%s'", count, query);
    return count;
}

int registry_save(void) {
    LOG_DEBUG_T("Registry", "Save", "Enter", "saving registry");

    LOG_DEBUG_T("Registry", "Save", "Lock", "acquiring registry lock");
    pthread_mutex_lock(&g_registry_lock);

    const char *path = get_index_path();
    ensure_registry_dir();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "registry_id", "lingos_registry");
    cJSON_AddNumberToObject(root, "updated_at", (double)time(NULL));

    cJSON *entries = cJSON_CreateArray();
    for (int i = 0; i < g_entry_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_entries[i].id);
        cJSON_AddNumberToObject(item, "type", g_entries[i].type);
        cJSON_AddStringToObject(item, "name", g_entries[i].name);
        cJSON_AddStringToObject(item, "version", g_entries[i].version);
        cJSON_AddNumberToObject(item, "status", g_entries[i].status);
        cJSON_AddStringToObject(item, "path", g_entries[i].path);
        cJSON_AddNumberToObject(item, "created_at", (double)g_entries[i].created_at);
        cJSON_AddNumberToObject(item, "updated_at", (double)g_entries[i].updated_at);
        cJSON_AddItemToArray(entries, item);
    }
    cJSON_AddItemToObject(root, "entries", entries);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_DEBUG_T("Registry", "Save", "Unlock", "print failed, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Save", "PrintFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    char temp_path[512];
    safe_snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *fp = fopen(temp_path, "w");
    if (!fp) {
        free(json_str);
        LOG_DEBUG_T("Registry", "Save", "Unlock", "open failed, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Save", "OpenFail", "cannot write %s", temp_path);
        return -1;
    }

    if (fprintf(fp, "%s\n", json_str) < 0) {
        fclose(fp);
        free(json_str);
        LOG_DEBUG_T("Registry", "Save", "Unlock", "write failed, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Save", "WriteFail", "write failed");
        return -1;
    }

    if (fclose(fp) != 0) {
        free(json_str);
        LOG_DEBUG_T("Registry", "Save", "Unlock", "close failed, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Save", "CloseFail", "fclose failed");
        unlink(temp_path);
        return -1;
    }
    free(json_str);

    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        LOG_DEBUG_T("Registry", "Save", "Unlock", "rename failed, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_ERROR_T("Registry", "Save", "RenameFail", "rename %s to %s failed", temp_path, path);
        return -1;
    }

    LOG_DEBUG_T("Registry", "Save", "Unlock", "releasing lock");
    pthread_mutex_unlock(&g_registry_lock);

    LOG_DEBUG_T("Registry", "Save", "OK", "saved %d entries", g_entry_count);
    return 0;
}

int registry_load(void) {
    LOG_DEBUG_T("Registry", "Load", "Enter", "loading registry with timeout %d seconds", REGISTRY_LOAD_TIMEOUT);

    const char *path = get_index_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Registry", "Load", "NotFound", "registry file not found");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        LOG_ERROR_T("Registry", "Load", "MallocFail", "malloc failed");
        return -1;
    }

    /* ============================================================
     * 关键修复：在 sigsetjmp 之前加锁，确保超时分支能正确解锁
     * ============================================================ */
    LOG_DEBUG_T("Registry", "Load", "Lock", "acquiring registry lock before timeout setup");
    pthread_mutex_lock(&g_registry_lock);
    LOG_DEBUG_T("Registry", "Load", "Lock", "registry lock acquired");

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = registry_alarm_handler;
    sigaction(SIGALRM, &sa, &old_sa);

    g_timeout_occurred = 0;

    if (sigsetjmp(g_timeout_env, 1) == 0) {
        LOG_DEBUG_T("Registry", "Load", "Timeout", "starting alarm (%d seconds)", REGISTRY_LOAD_TIMEOUT);
        alarm(REGISTRY_LOAD_TIMEOUT);
        /* 实际读取操作 */
        size_t read_len = fread(buf, 1, len, fp);
        buf[len] = '\0';
        alarm(0);
        sigaction(SIGALRM, &old_sa, NULL);

        if (g_timeout_occurred) {
            LOG_WARN_T("Registry", "Load", "Timeout", "loading timed out after %d seconds, unlocking and returning",
                       REGISTRY_LOAD_TIMEOUT);
            /* ============================================================
             * 关键修复：超时跳转后必须释放锁！
             * ============================================================ */
            free(buf);
            fclose(fp);
            LOG_DEBUG_T("Registry", "Load", "Unlock", "releasing lock after timeout");
            pthread_mutex_unlock(&g_registry_lock);
            LOG_WARN_T("Registry", "Load", "Timeout", "loading timed out, lock released");
            return -1;
        }

        fclose(fp);

        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            LOG_DEBUG_T("Registry", "Load", "Unlock", "parse failed, releasing lock");
            pthread_mutex_unlock(&g_registry_lock);
            LOG_ERROR_T("Registry", "Load", "ParseFail", "invalid JSON");
            return -1;
        }

        cJSON *entries = cJSON_GetObjectItem(root, "entries");
        if (!entries || !cJSON_IsArray(entries)) {
            cJSON_Delete(root);
            LOG_DEBUG_T("Registry", "Load", "Unlock", "no entries array, releasing lock");
            pthread_mutex_unlock(&g_registry_lock);
            LOG_ERROR_T("Registry", "Load", "NoEntries", "missing entries array");
            return -1;
        }

        g_entry_count = 0;
        int size = cJSON_GetArraySize(entries);
        LOG_DEBUG_T("Registry", "Load", "Parsing", "parsing %d entries", size);
        for (int i = 0; i < size && i < MAX_ENTRIES; i++) {
            cJSON *item = cJSON_GetArrayItem(entries, i);
            if (!item) continue;

            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *type = cJSON_GetObjectItem(item, "type");
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *version = cJSON_GetObjectItem(item, "version");
            cJSON *status = cJSON_GetObjectItem(item, "status");
            cJSON *path = cJSON_GetObjectItem(item, "path");
            cJSON *created = cJSON_GetObjectItem(item, "created_at");
            cJSON *updated = cJSON_GetObjectItem(item, "updated_at");

            if (id && cJSON_IsString(id)) {
                registry_entry_t *e = &g_entries[g_entry_count];
                memset(e, 0, sizeof(registry_entry_t));
                safe_strncpy(e->id, id->valuestring, sizeof(e->id));
                if (type && cJSON_IsNumber(type)) e->type = (registry_type_t)type->valueint;
                if (name && cJSON_IsString(name)) safe_strncpy(e->name, name->valuestring, sizeof(e->name));
                if (version && cJSON_IsString(version)) safe_strncpy(e->version, version->valuestring, sizeof(e->version));
                if (status && cJSON_IsNumber(status)) e->status = (registry_status_t)status->valueint;
                if (path && cJSON_IsString(path)) safe_strncpy(e->path, path->valuestring, sizeof(e->path));
                if (created && cJSON_IsNumber(created)) e->created_at = (time_t)created->valuedouble;
                if (updated && cJSON_IsNumber(updated)) e->updated_at = (time_t)updated->valuedouble;
                g_entry_count++;
            }
        }

        cJSON_Delete(root);

        LOG_DEBUG_T("Registry", "Load", "Unlock", "load successful, releasing lock");
        pthread_mutex_unlock(&g_registry_lock);

        LOG_INFO_T("Registry", "Load", "OK", "loaded %d entries", g_entry_count);
        return 0;
    } else {
        /* 超时跳转到这里 */
        LOG_WARN_T("Registry", "Load", "Timeout", "jumped to timeout handler");
        alarm(0);
        sigaction(SIGALRM, &old_sa, NULL);
        free(buf);
        fclose(fp);
        /* ============================================================
         * 关键修复：超时跳转后必须释放锁！
         * ============================================================ */
        LOG_DEBUG_T("Registry", "Load", "Unlock", "releasing lock after timeout jump");
        pthread_mutex_unlock(&g_registry_lock);
        LOG_WARN_T("Registry", "Load", "Timeout", "loading timed out, lock released");
        return -1;
    }
}

int registry_reload(void) {
    LOG_INFO_T("Registry", "Reload", "Enter", "reloading registry");
    int ret = registry_load();
    return ret;
}

int registry_on_change(registry_change_cb cb, void *user_data) {
    g_change_cb = cb;
    g_change_user_data = user_data;
    LOG_DEBUG_T("Registry", "OnChange", "OK", "callback registered");
    return 0;
}

/* 供 config_loader 调用的热重载函数 */
int registry_reload_from_json(const cJSON *root) {
    (void)root;
    return registry_reload();
}

void registry_reload_notify(void) {
    LOG_INFO_T("Registry", "ReloadNotify", "OK", "registry reloaded via config_loader");
}

/* ============================================================
 * 自检集成 API
 * ============================================================ */

int registry_get_selfcheck_list(registry_entry_t **out, int max_count) {
    return registry_list(REG_TYPE_SELFCHECK, out, max_count);
}

int registry_run_selfcheck(const char *module_name) {
    LOG_INFO_T("Registry", "RunSelfCheck", "Enter", "module='%s'", module_name ? module_name : "(null)");
    LOG_DEBUG_T("Registry", "RunSelfCheck", "Stub", "selfcheck callback not yet implemented");
    return 0;
}