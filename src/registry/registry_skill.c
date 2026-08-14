/**
 * @file    registry_skill.c
 * @brief   技能注册与查询（外置技能加载）
 * @version LN-B-5.0.0.0
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
#include <dirent.h>
#include <sys/stat.h>

#define SKILL_REGISTRY_DIR "/registry/skills"
#define BUILTIN_DIR "/registry/skills/builtin"
#define CUSTOM_DIR "/registry/skills/custom"
#define STORE_DIR "/registry/skills/store"

/**
 * @brief 加载指定目录下的所有技能 JSON 文件到注册表
 * @param dir 技能目录（相对于 /LINGOS）
 * @return 加载的数量
 */
int registry_skill_load_from_dir(const char *dir) {
    LOG_INFO_T("RegistrySkill", "LoadDir", "Enter", "dir='%s'", dir ? dir : "(null)");

    if (!dir) return -1;

    const char *root = lingos_data_root();
    char full_dir[512];
    safe_snprintf(full_dir, sizeof(full_dir), "%s%s", root, dir);

    DIR *d = opendir(full_dir);
    if (!d) {
        LOG_WARN_T("RegistrySkill", "LoadDir", "OpenFail", "cannot open %s", full_dir);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || (strcmp(dot, ".json") != 0 && strcmp(dot, ".md") != 0)) continue;

        char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", full_dir, entry->d_name);
        FILE *fp = fopen(full_path, "r");
        if (!fp) continue;

        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = malloc(len + 1);
        if (!buf) { fclose(fp); continue; }
        fread(buf, 1, len, fp);
        buf[len] = '\0';
        fclose(fp);

        cJSON *root_json = cJSON_Parse(buf);
        free(buf);
        if (!root_json) {
            LOG_WARN_T("RegistrySkill", "LoadDir", "ParseFail", "invalid JSON in %s", entry->d_name);
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(root_json, "name");
        cJSON *id = cJSON_GetObjectItem(root_json, "id");
        cJSON *version = cJSON_GetObjectItem(root_json, "version");
        cJSON *risk = cJSON_GetObjectItem(root_json, "risk");
        cJSON *handler = cJSON_GetObjectItem(root_json, "handler");
        cJSON *description = cJSON_GetObjectItem(root_json, "description");

        if (!name || !cJSON_IsString(name)) {
            cJSON_Delete(root_json);
            LOG_WARN_T("RegistrySkill", "LoadDir", "NoName", "skill missing 'name' in %s", entry->d_name);
            continue;
        }

        /* 构造注册条目 */
        registry_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        if (id && cJSON_IsString(id)) {
            safe_strncpy(entry.id, id->valuestring, sizeof(entry.id));
        } else {
            safe_snprintf(entry.id, sizeof(entry.id), "skill:%s", name->valuestring);
        }
        entry.type = REG_TYPE_SKILL;
        safe_strncpy(entry.name, name->valuestring, sizeof(entry.name));
        if (version && cJSON_IsString(version)) {
            safe_strncpy(entry.version, version->valuestring, sizeof(entry.version));
        } else {
            safe_strncpy(entry.version, "1.0.0", sizeof(entry.version));
        }
        entry.status = REG_STATUS_ACTIVE;
        safe_strncpy(entry.path, full_path, sizeof(entry.path));
        entry.metadata = (void*)root_json;  /* 存储整个 JSON */

        if (registry_register(&entry) == 0) count++;
        else cJSON_Delete(root_json);
    }

    closedir(d);
    LOG_INFO_T("RegistrySkill", "LoadDir", "OK", "loaded %d skills from %s", count, dir);
    return count;
}

/**
 * @brief 加载所有技能（builtin + custom + store）
 */
int registry_skill_load_all(void) {
    int total = 0;
    total += registry_skill_load_from_dir(BUILTIN_DIR);
    total += registry_skill_load_from_dir(CUSTOM_DIR);
    total += registry_skill_load_from_dir(STORE_DIR);
    return total;
}

/**
 * @brief 获取技能定义（返回 cJSON 指针，调用者不应释放）
 */
cJSON* registry_skill_get_definition(const char *skill_id) {
    const registry_entry_t *entry = registry_get(skill_id);
    if (!entry) return NULL;
    return (cJSON*)entry->metadata;
}