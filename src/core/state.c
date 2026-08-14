/**
 * @file    state.c
 * @brief   组件状态管理（基于 /LINGOS/state/components/*.json），并同步注册表
 * @version LN-B-5.0.0.0
 * @changes 新增注册表同步；安全字符串替换
 */

#include "state.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "cJSON.h"
#include "../registry/registry.h"
#include "safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#define STATE_DIR "/state/components"

static const char* get_state_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, STATE_DIR);
    }
    return path;
}

static int ensure_dir(void) {
    const char *dir = get_state_dir();
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("State", "Init", "MkdirFail", "cannot create %s", dir);
            return -1;
        }
    }
    return 0;
}

int component_state_init(void) {
    ensure_dir();
    LOG_INFO_T("State", "Init", "OK", "component state directory ready");
    return 0;
}

static int write_state_file(const char *id, const component_state_t *state) {
    const char *dir = get_state_dir();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", state->id);
    cJSON_AddStringToObject(root, "name", state->name);
    cJSON_AddStringToObject(root, "version", state->version);
    cJSON_AddBoolToObject(root, "enabled", state->enabled);
    cJSON_AddBoolToObject(root, "running", state->running);
    cJSON_AddStringToObject(root, "description", state->description);
    cJSON_AddNumberToObject(root, "timestamp", (double)state->timestamp);
    char *json = cJSON_PrintUnformatted(root);
    fprintf(fp, "%s\n", json);
    fclose(fp);
    free(json);
    cJSON_Delete(root);

    /* 同步到注册表 */
    registry_entry_t reg_entry;
    memset(&reg_entry, 0, sizeof(reg_entry));
    safe_snprintf(reg_entry.id, sizeof(reg_entry.id), "component:%s", id);
    reg_entry.type = REG_TYPE_COMPONENT;
    safe_strncpy(reg_entry.name, state->name, sizeof(reg_entry.name));
    safe_strncpy(reg_entry.version, state->version, sizeof(reg_entry.version));
    reg_entry.status = state->running ? REG_STATUS_ACTIVE : REG_STATUS_INACTIVE;
    registry_register(&reg_entry);

    return 0;
}

int component_state_register(const component_state_t *state) {
    if (!state || !state->id[0]) return -1;
    ensure_dir();
    component_state_t existing;
    if (component_state_get(state->id, &existing) == 0) {
        component_state_t updated = *state;
        updated.timestamp = time(NULL);
        return write_state_file(state->id, &updated);
    } else {
        component_state_t new_state = *state;
        new_state.timestamp = time(NULL);
        return write_state_file(state->id, &new_state);
    }
}

int component_state_set_running(const char *id, int running) {
    component_state_t state;
    if (component_state_get(id, &state) != 0) return -1;
    state.running = running;
    state.timestamp = time(NULL);
    return write_state_file(id, &state);
}

int component_state_get(const char *id, component_state_t *out) {
    if (!id || !out) return -1;
    const char *dir = get_state_dir();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;
    memset(out, 0, sizeof(component_state_t));
    cJSON *id_json = cJSON_GetObjectItem(root, "id");
    if (id_json && id_json->valuestring) safe_strncpy(out->id, id_json->valuestring, sizeof(out->id));
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name && name->valuestring) safe_strncpy(out->name, name->valuestring, sizeof(out->name));
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    if (ver && ver->valuestring) safe_strncpy(out->version, ver->valuestring, sizeof(out->version));
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (en) out->enabled = cJSON_IsTrue(en);
    cJSON *run = cJSON_GetObjectItem(root, "running");
    if (run) out->running = cJSON_IsTrue(run);
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    if (desc && desc->valuestring) safe_strncpy(out->description, desc->valuestring, sizeof(out->description));
    cJSON *ts = cJSON_GetObjectItem(root, "timestamp");
    if (ts) out->timestamp = (uint64_t)ts->valuedouble;
    cJSON_Delete(root);
    return 0;
}

void component_state_scan(void) {
    const char *dir = get_state_dir();
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    LOG_INFO_T("State", "Scan", "Start", "scanning components...");
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0) continue;
        char id[64];
        int len = dot - ent->d_name;
        if (len >= 64) len = 63;
        safe_strncpy(id, ent->d_name, len);
        id[len] = '\0';
        component_state_t state;
        if (component_state_get(id, &state) == 0) {
            LOG_DEBUG_T("State", "Scan", "Found", "id=%s name=%s version=%s enabled=%d running=%d",
                        state.id, state.name, state.version, state.enabled, state.running);
        }
    }
    closedir(d);
    LOG_INFO_T("State", "Scan", "Done", "scan completed");
}