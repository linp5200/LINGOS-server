/**
 * @file    ai_reminder_store.c
 * @brief   提醒持久化存储（JSON 文件）
 * @version LN-B-4.2.0.0
 */

#include "ai_reminder.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#define INDEX_FILE "/data/reminders/index.json"
#define REMINDER_DIR "/data/reminders"

static cJSON *g_index_root = NULL;
static int g_index_loaded = 0;

/* ============================================================
 * 内部辅助：获取目录和文件路径
 * ============================================================ */

static const char* get_reminder_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, REMINDER_DIR);
    }
    return path;
}

static const char* get_index_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, INDEX_FILE);
    }
    return path;
}

/* ============================================================
 * 内部辅助：提醒与 JSON 互转
 * ============================================================ */

static cJSON* reminder_to_json(const reminder_t *r) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "id", r->id);
    cJSON_AddStringToObject(obj, "content", r->content);
    cJSON_AddNumberToObject(obj, "trigger_time", (double)r->trigger_time);
    cJSON_AddNumberToObject(obj, "created_at", (double)r->created_at);
    cJSON_AddNumberToObject(obj, "triggered_at", (double)r->triggered_at);
    cJSON_AddNumberToObject(obj, "status", r->status);
    cJSON_AddNumberToObject(obj, "repeat", r->repeat);
    cJSON_AddNumberToObject(obj, "repeat_interval", r->repeat_interval);
    cJSON_AddStringToObject(obj, "session_id", r->session_id);

    return obj;
}

static int json_to_reminder(cJSON *obj, reminder_t *out) {
    if (!obj || !out) return -1;

    memset(out, 0, sizeof(reminder_t));

    cJSON *id = cJSON_GetObjectItem(obj, "id");
    cJSON *content = cJSON_GetObjectItem(obj, "content");
    cJSON *trigger_time = cJSON_GetObjectItem(obj, "trigger_time");
    cJSON *created_at = cJSON_GetObjectItem(obj, "created_at");
    cJSON *triggered_at = cJSON_GetObjectItem(obj, "triggered_at");
    cJSON *status = cJSON_GetObjectItem(obj, "status");
    cJSON *repeat = cJSON_GetObjectItem(obj, "repeat");
    cJSON *repeat_interval = cJSON_GetObjectItem(obj, "repeat_interval");
    cJSON *session_id = cJSON_GetObjectItem(obj, "session_id");

    if (id && cJSON_IsString(id)) safe_strncpy(out->id, id->valuestring, sizeof(out->id));
    if (content && cJSON_IsString(content)) safe_strncpy(out->content, content->valuestring, sizeof(out->content));
    if (trigger_time && cJSON_IsNumber(trigger_time)) out->trigger_time = (time_t)trigger_time->valuedouble;
    if (created_at && cJSON_IsNumber(created_at)) out->created_at = (time_t)created_at->valuedouble;
    if (triggered_at && cJSON_IsNumber(triggered_at)) out->triggered_at = (time_t)triggered_at->valuedouble;
    if (status && cJSON_IsNumber(status)) out->status = (reminder_status_t)status->valueint;
    if (repeat && cJSON_IsNumber(repeat)) out->repeat = repeat->valueint;
    if (repeat_interval && cJSON_IsNumber(repeat_interval)) out->repeat_interval = repeat_interval->valueint;
    if (session_id && cJSON_IsString(session_id)) safe_strncpy(out->session_id, session_id->valuestring, sizeof(out->session_id));

    return 0;
}

/* ============================================================
 * 内部辅助：加载索引
 * ============================================================ */

static int load_index(void) {
    if (g_index_loaded) return 0;

    const char *path = get_index_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("ReminderStore", "LoadIndex", "NotFound", "index file not found");
        g_index_root = cJSON_CreateObject();
        cJSON_AddItemToObject(g_index_root, "reminders", cJSON_CreateArray());
        g_index_loaded = 1;
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    g_index_root = cJSON_Parse(buf);
    free(buf);

    if (!g_index_root) {
        LOG_ERROR_T("ReminderStore", "LoadIndex", "ParseFail", "invalid JSON");
        g_index_root = cJSON_CreateObject();
        cJSON_AddItemToObject(g_index_root, "reminders", cJSON_CreateArray());
    }

    /* 确保 reminders 数组存在 */
    if (!cJSON_GetObjectItem(g_index_root, "reminders")) {
        cJSON_AddItemToObject(g_index_root, "reminders", cJSON_CreateArray());
    }

    g_index_loaded = 1;
    LOG_DEBUG_T("ReminderStore", "LoadIndex", "OK", "index loaded");
    return 0;
}

/* ============================================================
 * 内部辅助：保存索引
 * ============================================================ */

static int save_index(void) {
    if (!g_index_loaded || !g_index_root) {
        return -1;
    }

    const char *path = get_index_path();
    char *json_str = cJSON_PrintUnformatted(g_index_root);
    if (!json_str) {
        LOG_ERROR_T("ReminderStore", "SaveIndex", "PrintFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        LOG_ERROR_T("ReminderStore", "SaveIndex", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    LOG_DEBUG_T("ReminderStore", "SaveIndex", "OK", "index saved");
    return 0;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int reminder_store_load(void) {
    return load_index();
}

int reminder_store_save(const reminder_t *reminder) {
    LOG_DEBUG_T("ReminderStore", "Save", "Enter", "id='%s'", reminder ? reminder->id : "(null)");

    if (!reminder) {
        LOG_ERROR_T("ReminderStore", "Save", "Invalid", "reminder is NULL");
        return -1;
    }

    if (load_index() != 0) {
        LOG_ERROR_T("ReminderStore", "Save", "LoadFail", "failed to load index");
        return -1;
    }

    cJSON *reminders = cJSON_GetObjectItem(g_index_root, "reminders");
    if (!reminders) {
        reminders = cJSON_CreateArray();
        cJSON_AddItemToObject(g_index_root, "reminders", reminders);
    }

    /* 查找是否已存在，更新或添加 */
    int found = 0;
    int size = cJSON_GetArraySize(reminders);
    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(reminders, i);
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (id && cJSON_IsString(id) && strcmp(id->valuestring, reminder->id) == 0) {
            /* 替换现有条目 */
            cJSON_DeleteItemFromArray(reminders, i);
            cJSON *new_item = reminder_to_json(reminder);
            if (new_item) {
                cJSON_AddItemToArray(reminders, new_item);
            }
            found = 1;
            break;
        }
    }

    if (!found) {
        cJSON *new_item = reminder_to_json(reminder);
        if (new_item) {
            cJSON_AddItemToArray(reminders, new_item);
        }
    }

    /* 同时保存提醒内容到单独文件（备用） */
    const char *dir = get_reminder_dir();
    char file_path[512];
    safe_snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, reminder->id);
    cJSON *content_obj = reminder_to_json(reminder);
    if (content_obj) {
        char *content_str = cJSON_PrintUnformatted(content_obj);
        cJSON_Delete(content_obj);
        if (content_str) {
            FILE *fp = fopen(file_path, "w");
            if (fp) {
                fprintf(fp, "%s\n", content_str);
                fclose(fp);
            }
            free(content_str);
        }
    }

    return save_index();
}

int reminder_store_delete(const char *id) {
    LOG_DEBUG_T("ReminderStore", "Delete", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("ReminderStore", "Delete", "Invalid", "id is NULL or empty");
        return -1;
    }

    if (load_index() != 0) {
        LOG_ERROR_T("ReminderStore", "Delete", "LoadFail", "failed to load index");
        return -1;
    }

    cJSON *reminders = cJSON_GetObjectItem(g_index_root, "reminders");
    if (!reminders) {
        return -1;
    }

    int found = 0;
    int size = cJSON_GetArraySize(reminders);
    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(reminders, i);
        cJSON *id_json = cJSON_GetObjectItem(item, "id");
        if (id_json && cJSON_IsString(id_json) && strcmp(id_json->valuestring, id) == 0) {
            cJSON_DeleteItemFromArray(reminders, i);
            found = 1;
            break;
        }
    }

    if (!found) {
        LOG_WARN_T("ReminderStore", "Delete", "NotFound", "reminder %s not in index", id);
        return -1;
    }

    /* 删除内容文件 */
    const char *dir = get_reminder_dir();
    char file_path[512];
    safe_snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, id);
    unlink(file_path);

    return save_index();
}

int reminder_store_get(const char *id, reminder_t *out) {
    LOG_DEBUG_T("ReminderStore", "Get", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id || !out) {
        LOG_ERROR_T("ReminderStore", "Get", "Invalid", "id=%p, out=%p", (void*)id, (void*)out);
        return -1;
    }

    if (load_index() != 0) {
        LOG_ERROR_T("ReminderStore", "Get", "LoadFail", "failed to load index");
        return -1;
    }

    cJSON *reminders = cJSON_GetObjectItem(g_index_root, "reminders");
    if (!reminders) {
        return -1;
    }

    int size = cJSON_GetArraySize(reminders);
    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(reminders, i);
        cJSON *id_json = cJSON_GetObjectItem(item, "id");
        if (id_json && cJSON_IsString(id_json) && strcmp(id_json->valuestring, id) == 0) {
            return json_to_reminder(item, out);
        }
    }

    LOG_WARN_T("ReminderStore", "Get", "NotFound", "reminder %s not found", id);
    return -1;
}

int reminder_store_list(reminder_t *out, int max_count, int include_triggered) {
    LOG_DEBUG_T("ReminderStore", "List", "Enter", "max_count=%d, include_triggered=%d",
                max_count, include_triggered);

    if (!out || max_count <= 0) {
        LOG_ERROR_T("ReminderStore", "List", "Invalid", "out=%p, max_count=%d", (void*)out, max_count);
        return -1;
    }

    if (load_index() != 0) {
        LOG_ERROR_T("ReminderStore", "List", "LoadFail", "failed to load index");
        return -1;
    }

    cJSON *reminders = cJSON_GetObjectItem(g_index_root, "reminders");
    if (!reminders) {
        return 0;
    }

    int count = 0;
    int size = cJSON_GetArraySize(reminders);
    for (int i = 0; i < size && count < max_count; i++) {
        cJSON *item = cJSON_GetArrayItem(reminders, i);
        if (!item) continue;

        /* 检查状态 */
        cJSON *status = cJSON_GetObjectItem(item, "status");
        int st = (status && cJSON_IsNumber(status)) ? status->valueint : 0;
        if (!include_triggered && st == REMINDER_STATUS_TRIGGERED) {
            continue;
        }
        if (st == REMINDER_STATUS_CANCELLED) {
            continue;
        }

        if (json_to_reminder(item, &out[count]) == 0) {
            count++;
        }
    }

    LOG_DEBUG_T("ReminderStore", "List", "OK", "returned %d reminders", count);
    return count;
}

int reminder_store_count(int include_triggered) {
    if (load_index() != 0) {
        return 0;
    }

    cJSON *reminders = cJSON_GetObjectItem(g_index_root, "reminders");
    if (!reminders) {
        return 0;
    }

    int count = 0;
    int size = cJSON_GetArraySize(reminders);
    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(reminders, i);
        if (!item) continue;

        cJSON *status = cJSON_GetObjectItem(item, "status");
        int st = (status && cJSON_IsNumber(status)) ? status->valueint : 0;
        if (!include_triggered && st == REMINDER_STATUS_TRIGGERED) {
            continue;
        }
        if (st == REMINDER_STATUS_CANCELLED) {
            continue;
        }
        count++;
    }
    return count;
}

void reminder_store_clear(void) {
    if (g_index_root) {
        cJSON_Delete(g_index_root);
        g_index_root = NULL;
    }
    g_index_loaded = 0;
}