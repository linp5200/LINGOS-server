/**
 * @file    rules_storage.c
 * @brief   规则存储（JSON）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程（存储失败不影响主流程）
 */

#include "rules_storage.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>
#include <sys/stat.h>

#define STORAGE_PATH "/system/config/rules.json"

/* ============================================================
 * 获取存储路径
 * ============================================================ */

static const char* get_storage_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, STORAGE_PATH);
    }
    return path;
}

/* ============================================================
 * 保存规则到存储
 * ============================================================ */

int rules_storage_save(const rule_t *rules, int count) {
    LOG_DEBUG_T("RulesStorage", "Save", "Enter", "count=%d", count);

    const char *path = get_storage_path();

    /* 确保目录存在 */
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    cJSON *root_json = cJSON_CreateObject();
    cJSON *rules_array = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        const rule_t *r = &rules[i];
        if (r->name[0] == '\0') continue;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", r->name);
        cJSON_AddStringToObject(item, "condition", r->condition);
        cJSON_AddNumberToObject(item, "action_count", r->action_count);

        cJSON *actions = cJSON_CreateArray();
        for (int j = 0; j < r->action_count; j++) {
            if (r->actions[j][0] != '\0') {
                cJSON_AddItemToArray(actions, cJSON_CreateString(r->actions[j]));
            }
        }
        cJSON_AddItemToObject(item, "actions", actions);

        cJSON_AddBoolToObject(item, "enabled", r->enabled);
        cJSON_AddNumberToObject(item, "created_at", (double)r->created_at);
        cJSON_AddNumberToObject(item, "last_triggered", (double)r->last_triggered);
        cJSON_AddNumberToObject(item, "trigger_count", r->trigger_count);
        cJSON_AddBoolToObject(item, "is_custom", r->is_custom);

        cJSON_AddItemToArray(rules_array, item);
    }

    cJSON_AddItemToObject(root_json, "rules", rules_array);
    cJSON_AddNumberToObject(root_json, "version", 1);
    cJSON_AddNumberToObject(root_json, "count", count);
    cJSON_AddNumberToObject(root_json, "updated_at", (double)time(NULL));

    char *json_str = cJSON_PrintUnformatted(root_json);
    cJSON_Delete(root_json);

    if (!json_str) {
        LOG_ERROR_T("RulesStorage", "Save", "PrintFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        LOG_ERROR_T("RulesStorage", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    LOG_INFO_T("RulesStorage", "Save", "OK", "saved %d rules", count);
    return 0;
}

/* ============================================================
 * 从存储加载规则
 * ============================================================ */

int rules_storage_load(rule_t *out, int max_count) {
    LOG_DEBUG_T("RulesStorage", "Load", "Enter", "max_count=%d", max_count);

    if (!out || max_count <= 0) return 0;

    const char *path = get_storage_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("RulesStorage", "Load", "NotFound", "storage not found");
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        LOG_ERROR_T("RulesStorage", "Load", "ParseFail", "invalid JSON");
        return 0;
    }

    cJSON *rules_array = cJSON_GetObjectItem(root, "rules");
    if (!rules_array || !cJSON_IsArray(rules_array)) {
        cJSON_Delete(root);
        LOG_WARN_T("RulesStorage", "Load", "NoRules", "no rules array");
        return 0;
    }

    int count = 0;
    int size_arr = cJSON_GetArraySize(rules_array);
    for (int i = 0; i < size_arr && count < max_count; i++) {
        cJSON *item = cJSON_GetArrayItem(rules_array, i);
        if (!item) continue;

        rule_t *r = &out[count];
        memset(r, 0, sizeof(rule_t));

        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (name && cJSON_IsString(name)) {
            safe_strncpy(r->name, name->valuestring, sizeof(r->name));
        } else {
            continue;
        }

        cJSON *condition = cJSON_GetObjectItem(item, "condition");
        if (condition && cJSON_IsString(condition)) {
            safe_strncpy(r->condition, condition->valuestring, sizeof(r->condition));
        }

        cJSON *action_count = cJSON_GetObjectItem(item, "action_count");
        if (action_count && cJSON_IsNumber(action_count)) {
            r->action_count = action_count->valueint;
            if (r->action_count > RULE_MAX_ACTIONS) r->action_count = RULE_MAX_ACTIONS;
        }

        cJSON *actions = cJSON_GetObjectItem(item, "actions");
        if (actions && cJSON_IsArray(actions)) {
            int acount = cJSON_GetArraySize(actions);
            for (int j = 0; j < acount && j < RULE_MAX_ACTIONS; j++) {
                cJSON *act = cJSON_GetArrayItem(actions, j);
                if (act && cJSON_IsString(act)) {
                    safe_strncpy(r->actions[j], act->valuestring, sizeof(r->actions[j]));
                }
            }
            if (acount > r->action_count) r->action_count = acount;
        }

        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        if (enabled && cJSON_IsBool(enabled)) r->enabled = cJSON_IsTrue(enabled);

        cJSON *created = cJSON_GetObjectItem(item, "created_at");
        if (created && cJSON_IsNumber(created)) r->created_at = (time_t)created->valuedouble;

        cJSON *last = cJSON_GetObjectItem(item, "last_triggered");
        if (last && cJSON_IsNumber(last)) r->last_triggered = (time_t)last->valuedouble;

        cJSON *trigger_count = cJSON_GetObjectItem(item, "trigger_count");
        if (trigger_count && cJSON_IsNumber(trigger_count)) r->trigger_count = trigger_count->valueint;

        cJSON *is_custom = cJSON_GetObjectItem(item, "is_custom");
        if (is_custom && cJSON_IsBool(is_custom)) r->is_custom = cJSON_IsTrue(is_custom);

        count++;
    }

    cJSON_Delete(root);
    LOG_INFO_T("RulesStorage", "Load", "OK", "loaded %d rules", count);
    return count;
}