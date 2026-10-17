/**
 * @file    permission_whitelist.c
 * @brief   权限白名单管理（默认禁用，应用授权）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程
 */

#include "permission_whitelist.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <sys/stat.h>   
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>
#include <pthread.h>

#define WHITELIST_PATH "/system/config/permission_whitelist.json"

static pthread_mutex_t g_whitelist_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_whitelist_root = NULL;
static int g_loaded = 0;

/* ============================================================
 * 获取配置文件路径
 * ============================================================ */

static const char* get_whitelist_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, WHITELIST_PATH);
    }
    return path;
}

/* ============================================================
 * 加载白名单
 * ============================================================ */

int permission_whitelist_load(void) {
    LOG_DEBUG_T("PermWhitelist", "Load", "Enter", "loading whitelist");

    pthread_mutex_lock(&g_whitelist_lock);

    if (g_whitelist_root) {
        cJSON_Delete(g_whitelist_root);
        g_whitelist_root = NULL;
    }

    const char *path = get_whitelist_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("PermWhitelist", "Load", "NotFound", "whitelist not found, creating default");
        g_whitelist_root = cJSON_CreateObject();
        cJSON_AddItemToObject(g_whitelist_root, "apps", cJSON_CreateArray());
        g_loaded = 1;
        pthread_mutex_unlock(&g_whitelist_lock);
        /* 【2026-10-17 修复】原实现在持锁状态下调用 save()（同一非递归互斥锁）
         * → 冷启动（白名单文件不存在）时自死锁，lingosd 卡死在 init，
         *   导致 8080/2939 永远不监听、容器/新设备首启失败。
         *   改为：先解锁，再保存。 */
        permission_whitelist_save();
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    g_whitelist_root = cJSON_Parse(buf);
    free(buf);

    if (!g_whitelist_root) {
        LOG_ERROR_T("PermWhitelist", "Load", "ParseFail", "invalid JSON");
        g_whitelist_root = cJSON_CreateObject();
        cJSON_AddItemToObject(g_whitelist_root, "apps", cJSON_CreateArray());
    }

    /* 确保 apps 数组存在 */
    if (!cJSON_GetObjectItem(g_whitelist_root, "apps")) {
        cJSON_AddItemToObject(g_whitelist_root, "apps", cJSON_CreateArray());
    }

    g_loaded = 1;
    pthread_mutex_unlock(&g_whitelist_lock);

    LOG_INFO_T("PermWhitelist", "Load", "OK", "whitelist loaded");
    return 0;
}

/* ============================================================
 * 保存白名单
 * ============================================================ */

int permission_whitelist_save(void) {
    LOG_DEBUG_T("PermWhitelist", "Save", "Enter", "saving whitelist");

    pthread_mutex_lock(&g_whitelist_lock);

    if (!g_whitelist_root) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    /* 确保目录存在 */
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    const char *path = get_whitelist_path();
    char *json_str = cJSON_PrintUnformatted(g_whitelist_root);
    if (!json_str) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    pthread_mutex_unlock(&g_whitelist_lock);

    LOG_INFO_T("PermWhitelist", "Save", "OK", "whitelist saved");
    return 0;
}

/* ============================================================
 * 检查应用是否有权限
 * ============================================================ */

int permission_whitelist_check(const char *app_id, permission_type_t perm) {
    LOG_DEBUG_T("PermWhitelist", "Check", "Enter", "app=%s, perm=%d", app_id ? app_id : "(null)", perm);

    if (!app_id || !*app_id) {
        LOG_WARN_T("PermWhitelist", "Check", "Invalid", "app_id is NULL");
        return 0;
    }

    pthread_mutex_lock(&g_whitelist_lock);

    if (!g_loaded || !g_whitelist_root) {
        pthread_mutex_unlock(&g_whitelist_lock);
        LOG_WARN_T("PermWhitelist", "Check", "NotLoaded", "whitelist not loaded");
        return 0;
    }

    cJSON *apps = cJSON_GetObjectItem(g_whitelist_root, "apps");
    if (!apps || !cJSON_IsArray(apps)) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return 0;
    }

    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        if (!app) continue;

        cJSON *id = cJSON_GetObjectItem(app, "id");
        if (!id || !cJSON_IsString(id)) continue;
        if (strcmp(id->valuestring, app_id) != 0) continue;

        cJSON *perms = cJSON_GetObjectItem(app, "permissions");
        if (!perms || !cJSON_IsArray(perms)) {
            pthread_mutex_unlock(&g_whitelist_lock);
            return 0;
        }

        int psize = cJSON_GetArraySize(perms);
        for (int j = 0; j < psize; j++) {
            cJSON *p = cJSON_GetArrayItem(perms, j);
            if (!p || !cJSON_IsString(p)) continue;
            if (strcmp(p->valuestring, "ALL") == 0) {
                pthread_mutex_unlock(&g_whitelist_lock);
                return 1;
            }
            if (permission_type_from_string(p->valuestring) == perm) {
                pthread_mutex_unlock(&g_whitelist_lock);
                return 1;
            }
        }

        pthread_mutex_unlock(&g_whitelist_lock);
        return 0;
    }

    pthread_mutex_unlock(&g_whitelist_lock);
    LOG_DEBUG_T("PermWhitelist", "Check", "NotFound", "app %s not in whitelist", app_id);
    return 0;
}

/* ============================================================
 * 授权应用
 * ============================================================ */

int permission_whitelist_grant(const char *app_id, permission_type_t perm) {
    LOG_INFO_T("PermWhitelist", "Grant", "Enter", "app=%s, perm=%d", app_id ? app_id : "(null)", perm);

    if (!app_id || !*app_id) {
        LOG_ERROR_T("PermWhitelist", "Grant", "Invalid", "app_id is NULL");
        return -1;
    }

    pthread_mutex_lock(&g_whitelist_lock);

    if (!g_loaded || !g_whitelist_root) {
        permission_whitelist_load();
        if (!g_loaded) {
            pthread_mutex_unlock(&g_whitelist_lock);
            return -1;
        }
    }

    cJSON *apps = cJSON_GetObjectItem(g_whitelist_root, "apps");
    if (!apps) {
        apps = cJSON_CreateArray();
        cJSON_AddItemToObject(g_whitelist_root, "apps", apps);
    }

    cJSON *app_obj = NULL;
    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        if (!app) continue;
        cJSON *id = cJSON_GetObjectItem(app, "id");
        if (id && cJSON_IsString(id) && strcmp(id->valuestring, app_id) == 0) {
            app_obj = app;
            break;
        }
    }

    if (!app_obj) {
        app_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(app_obj, "id", app_id);
        cJSON_AddItemToObject(app_obj, "permissions", cJSON_CreateArray());
        cJSON_AddItemToArray(apps, app_obj);
    }

    cJSON *perms = cJSON_GetObjectItem(app_obj, "permissions");
    if (!perms) {
        perms = cJSON_CreateArray();
        cJSON_AddItemToObject(app_obj, "permissions", perms);
    }

    const char *perm_str = permission_type_to_string(perm);
    if (perm_str) {
        cJSON_AddItemToArray(perms, cJSON_CreateString(perm_str));
    }

    pthread_mutex_unlock(&g_whitelist_lock);
    permission_whitelist_save();

    LOG_INFO_T("PermWhitelist", "Grant", "OK", "app %s granted permission %s", app_id, perm_str ? perm_str : "unknown");
    return 0;
}

/* ============================================================
 * 撤销应用权限
 * ============================================================ */

int permission_whitelist_revoke(const char *app_id, permission_type_t perm) {
    LOG_INFO_T("PermWhitelist", "Revoke", "Enter", "app=%s, perm=%d", app_id ? app_id : "(null)", perm);

    if (!app_id || !*app_id) {
        LOG_ERROR_T("PermWhitelist", "Revoke", "Invalid", "app_id is NULL");
        return -1;
    }

    pthread_mutex_lock(&g_whitelist_lock);

    if (!g_loaded || !g_whitelist_root) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    cJSON *apps = cJSON_GetObjectItem(g_whitelist_root, "apps");
    if (!apps) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        if (!app) continue;
        cJSON *id = cJSON_GetObjectItem(app, "id");
        if (!id || !cJSON_IsString(id)) continue;
        if (strcmp(id->valuestring, app_id) != 0) continue;

        cJSON *perms = cJSON_GetObjectItem(app, "permissions");
        if (!perms || !cJSON_IsArray(perms)) {
            pthread_mutex_unlock(&g_whitelist_lock);
            return 0;
        }

        const char *perm_str = permission_type_to_string(perm);
        if (!perm_str) {
            pthread_mutex_unlock(&g_whitelist_lock);
            return -1;
        }

        int psize = cJSON_GetArraySize(perms);
        for (int j = psize - 1; j >= 0; j--) {
            cJSON *p = cJSON_GetArrayItem(perms, j);
            if (!p || !cJSON_IsString(p)) continue;
            if (strcmp(p->valuestring, perm_str) == 0) {
                cJSON_DeleteItemFromArray(perms, j);
                pthread_mutex_unlock(&g_whitelist_lock);
                permission_whitelist_save();
                LOG_INFO_T("PermWhitelist", "Revoke", "OK", "app %s revoked permission %s", app_id, perm_str);
                return 0;
            }
        }

        pthread_mutex_unlock(&g_whitelist_lock);
        LOG_WARN_T("PermWhitelist", "Revoke", "NotFound", "permission %s not found for app %s", perm_str, app_id);
        return -1;
    }

    pthread_mutex_unlock(&g_whitelist_lock);
    LOG_WARN_T("PermWhitelist", "Revoke", "AppNotFound", "app %s not found", app_id);
    return -1;
}

/* ============================================================
 * 列出应用权限
 * ============================================================ */

int permission_whitelist_list(const char *app_id, char *out, size_t out_len) {
    LOG_DEBUG_T("PermWhitelist", "List", "Enter", "app=%s", app_id ? app_id : "(null)");

    if (!out || out_len == 0) {
        return -1;
    }

    out[0] = '\0';

    pthread_mutex_lock(&g_whitelist_lock);

    if (!g_loaded || !g_whitelist_root) {
        permission_whitelist_load();
        if (!g_loaded) {
            pthread_mutex_unlock(&g_whitelist_lock);
            return -1;
        }
    }

    cJSON *apps = cJSON_GetObjectItem(g_whitelist_root, "apps");
    if (!apps) {
        pthread_mutex_unlock(&g_whitelist_lock);
        safe_strncpy(out, "[]", out_len);
        return 0;
    }

    if (!app_id || !*app_id) {
        /* 列出所有应用 */
        char *json_str = cJSON_PrintUnformatted(apps);
        if (json_str) {
            safe_strncpy(out, json_str, out_len);
            free(json_str);
        }
        pthread_mutex_unlock(&g_whitelist_lock);
        return 0;
    }

    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        if (!app) continue;
        cJSON *id = cJSON_GetObjectItem(app, "id");
        if (!id || !cJSON_IsString(id)) continue;
        if (strcmp(id->valuestring, app_id) == 0) {
            char *json_str = cJSON_PrintUnformatted(app);
            if (json_str) {
                safe_strncpy(out, json_str, out_len);
                free(json_str);
            }
            pthread_mutex_unlock(&g_whitelist_lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&g_whitelist_lock);
    safe_strncpy(out, "{}", out_len);
    return 0;
}

/* ============================================================
 * 【2026-10-10 P0 修复】白名单获取（加载 + 首次冷启动播种）
 * ------------------------------------------------------------
 * 根因（先生环境实测）：permission_whitelist_load() 全仓 0 外部调用，
 *   白名单文件 /LINGOS/system/config/permission_whitelist.json 从未生成；
 *   load() 在文件缺失时只建 {"apps":[]} 空名单 →
 *   permission_whitelist_check("ai", …) 恒 0 →
 *   permission_check_file 恒 0 → syscall file_check 恒 allowed=false →
 *   skill_handlers._file_guard fail-closed → 文件类技能（读/写/删）全被拒。
 *
 * 本函数修复：显式加载；若为"文件不存在"的冷启动 → 播种内置应用基线授权
 *   并在日志中打印 seeded（不静默，便于追查）。
 *   授权范围按先生 2026-10-10 决策：ai 授文件读写执行 + 网络 + DNS + SYSCALL（靠审批链兜底）。
 * ============================================================ */

/* 为 app 添加权限；返回新增条数（已存在则不重复添加） */
static int wl_add_perms(cJSON *app, const permission_type_t *perms, int n) {
    cJSON *arr = cJSON_GetObjectItem(app, "permissions");
    if (!arr || !cJSON_IsArray(arr)) {
        arr = cJSON_CreateArray();
        if (!arr) return 0;
        cJSON_AddItemToObject(app, "permissions", arr);
    }
    int added = 0;
    for (int i = 0; i < n; i++) {
        const char *s = permission_type_to_string(perms[i]);
        if (!s) continue;
        int dup = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsString(it) && strcmp(it->valuestring, s) == 0) { dup = 1; break; }
        }
        if (!dup) { cJSON_AddItemToArray(arr, cJSON_CreateString(s)); added++; }
    }
    return added;
}

/* 追加/补全一个应用条目并授权；返回新增权限条数 */
static int wl_seed_app(const char *app_id, const char *desc,
                       const permission_type_t *perms, int n) {
    if (!g_whitelist_root) return 0;
    cJSON *apps = cJSON_GetObjectItem(g_whitelist_root, "apps");
    if (!apps || !cJSON_IsArray(apps)) {
        apps = cJSON_CreateArray();
        if (!apps) return 0;
        cJSON_AddItemToObject(g_whitelist_root, "apps", apps);
    }
    cJSON *app = NULL;
    cJSON_ArrayForEach(app, apps) {
        cJSON *id = cJSON_GetObjectItem(app, "id");
        if (id && cJSON_IsString(id) && strcmp(id->valuestring, app_id) == 0)
            return wl_add_perms(app, perms, n);   /* 已存在：只补权限，不覆盖用户配置 */
    }
    app = cJSON_CreateObject();
    if (!app) return 0;
    cJSON_AddStringToObject(app, "id", app_id);
    cJSON_AddStringToObject(app, "desc", desc);
    cJSON_AddItemToObject(app, "permissions", cJSON_CreateArray());
    cJSON_AddItemToArray(apps, app);
    return wl_add_perms(app, perms, n);
}

int permission_whitelist_seed_defaults(void) {
    if (!g_loaded) {
        if (permission_whitelist_load() != 0) return -1;
    }
    pthread_mutex_lock(&g_whitelist_lock);
    if (!g_whitelist_root) {
        pthread_mutex_unlock(&g_whitelist_lock);
        return -1;
    }

    /* ① AI Agent（经 daemon.sock 执行面）——读写执行 + 网络 + DNS + SYSCALL */
    static const permission_type_t AI_PERMS[] = {
        PERM_FILE_READ, PERM_FILE_WRITE, PERM_FILE_DELETE, PERM_FILE_EXEC, PERM_FILE_LIST,
        PERM_NET_CONNECT, PERM_NET_DNS, PERM_NET_BIND, PERM_SYSCALL
    };
    /* ② 视觉守护——摄像头 */
    static const permission_type_t VISIOND_PERMS[] = { PERM_CAMERA, PERM_FILE_WRITE };
    /* ③ 语音守护——麦克风 */
    static const permission_type_t VOICED_PERMS[] = { PERM_MICROPHONE, PERM_FILE_WRITE };
    /* ④ 监控系统守护（2026-10-10 拆分为独立进程）——摄像头采集 + 快照/录像落盘 */
    static const permission_type_t MONITOR_PERMS[] = { PERM_CAMERA, PERM_FILE_READ, PERM_FILE_WRITE };

    int n = 0;
    n += wl_seed_app("ai",      "AI Agent (daemon.sock exec plane)", AI_PERMS,
                     (int)(sizeof(AI_PERMS) / sizeof(AI_PERMS[0])));
    n += wl_seed_app("visiond", "vision daemon (legacy camera capture)", VISIOND_PERMS,
                     (int)(sizeof(VISIOND_PERMS) / sizeof(VISIOND_PERMS[0])));
    n += wl_seed_app("monitor", "monitor daemon (camera capture + storage)", MONITOR_PERMS,
                     (int)(sizeof(MONITOR_PERMS) / sizeof(MONITOR_PERMS[0])));
    n += wl_seed_app("voiced",  "voice daemon (microphone)",         VOICED_PERMS,
                     (int)(sizeof(VOICED_PERMS) / sizeof(VOICED_PERMS[0])));
    pthread_mutex_unlock(&g_whitelist_lock);

    if (n > 0) {
        permission_whitelist_save();
        LOG_INFO_T("PermWhitelist", "Seed", "Done", "seeded=%d perms for ai/visiond/voiced", n);
    } else {
        LOG_DEBUG_T("PermWhitelist", "Seed", "Noop", "defaults already present");
    }
    return 0;
}

/* 启动期显式加载（此前全仓无调用者 → 白名单从未加载） */
int permission_whitelist_acquire(void) {
    if (g_loaded) {
        LOG_DEBUG_T("PermWhitelist", "Acquire", "Already", "whitelist already loaded");
        return 0;
    }
    const char *path = get_whitelist_path();
    struct stat st;
    int existed = (stat(path, &st) == 0);
    if (permission_whitelist_load() != 0) {
        LOG_ERROR_T("PermWhitelist", "Acquire", "LoadFail", "cannot load whitelist");
        return -1;
    }
    if (!existed) {
        LOG_WARN_T("PermWhitelist", "Acquire", "ColdStart", "whitelist absent -> seeding defaults");
        return permission_whitelist_seed_defaults();
    }
    LOG_INFO_T("PermWhitelist", "Acquire", "OK", "whitelist loaded from disk");
    return 0;
}

/* ============================================================
 * 权限类型转换
 * ============================================================ */

const char* permission_type_to_string(permission_type_t perm) {
    switch (perm) {
        case PERM_FILE_READ:   return "FILE_READ";
        case PERM_FILE_WRITE:  return "FILE_WRITE";
        case PERM_FILE_DELETE: return "FILE_DELETE";
        case PERM_FILE_EXEC:   return "FILE_EXEC";
        case PERM_FILE_LIST:   return "FILE_LIST";
        case PERM_NET_CONNECT: return "NET_CONNECT";
        case PERM_NET_BIND:    return "NET_BIND";
        case PERM_NET_DNS:     return "NET_DNS";
        case PERM_MEM_ALLOC:   return "MEM_ALLOC";
        case PERM_MEM_MAP:     return "MEM_MAP";
        case PERM_CPU_QUOTA:   return "CPU_QUOTA";
        case PERM_CPU_PRIORITY:return "CPU_PRIORITY";
        case PERM_CAMERA:      return "CAMERA";
        case PERM_MICROPHONE:  return "MICROPHONE";
        case PERM_BLUETOOTH:   return "BLUETOOTH";
        case PERM_USB:         return "USB";
        case PERM_SERIAL:      return "SERIAL";
        case PERM_SYSCALL:     return "SYSCALL";
        default:               return NULL;
    }
}

permission_type_t permission_type_from_string(const char *str) {
    if (!str) return PERM_UNKNOWN;
    if (strcmp(str, "FILE_READ") == 0) return PERM_FILE_READ;
    if (strcmp(str, "FILE_WRITE") == 0) return PERM_FILE_WRITE;
    if (strcmp(str, "FILE_DELETE") == 0) return PERM_FILE_DELETE;
    if (strcmp(str, "FILE_EXEC") == 0) return PERM_FILE_EXEC;
    if (strcmp(str, "FILE_LIST") == 0) return PERM_FILE_LIST;
    if (strcmp(str, "NET_CONNECT") == 0) return PERM_NET_CONNECT;
    if (strcmp(str, "NET_BIND") == 0) return PERM_NET_BIND;
    if (strcmp(str, "NET_DNS") == 0) return PERM_NET_DNS;
    if (strcmp(str, "MEM_ALLOC") == 0) return PERM_MEM_ALLOC;
    if (strcmp(str, "MEM_MAP") == 0) return PERM_MEM_MAP;
    if (strcmp(str, "CPU_QUOTA") == 0) return PERM_CPU_QUOTA;
    if (strcmp(str, "CPU_PRIORITY") == 0) return PERM_CPU_PRIORITY;
    if (strcmp(str, "CAMERA") == 0) return PERM_CAMERA;
    if (strcmp(str, "MICROPHONE") == 0) return PERM_MICROPHONE;
    if (strcmp(str, "BLUETOOTH") == 0) return PERM_BLUETOOTH;
    if (strcmp(str, "USB") == 0) return PERM_USB;
    if (strcmp(str, "SERIAL") == 0) return PERM_SERIAL;
    if (strcmp(str, "SYSCALL") == 0) return PERM_SYSCALL;
    if (strcmp(str, "ALL") == 0) return PERM_ALL;
    return PERM_UNKNOWN;
}