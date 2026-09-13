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

        char full_path[512];
        /* 【0.6.0】支持两种布局：
         *   ① 平铺文件：<dir>/<name>.json / .md
         *   ② 技能包目录：<dir>/<name>/skill.json（Python 侧另有 SKILL.md 规范） */
        char *dot = strrchr(entry->d_name, '.');
        if (dot && (strcmp(dot, ".json") == 0 || strcmp(dot, ".md") == 0)) {
            safe_snprintf(full_path, sizeof(full_path), "%s/%s", full_dir, entry->d_name);
        } else {
            char sub_json[512];
            safe_snprintf(sub_json, sizeof(sub_json), "%s/%s/skill.json", full_dir, entry->d_name);
            if (access(sub_json, F_OK) == 0) {
                safe_snprintf(full_path, sizeof(full_path), "%s", sub_json);
            } else {
                continue;   /* 无 skill.json 的子目录——跳过 */
            }
        }

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
 *        【0.6.0 接线】加载后同步导出技能索引（单一事实源镜像）
 */
int registry_skill_load_all(void) {
    int total = 0;
    total += registry_skill_load_from_dir(BUILTIN_DIR);
    total += registry_skill_load_from_dir(CUSTOM_DIR);
    total += registry_skill_load_from_dir(STORE_DIR);
    LOG_INFO_T("RegistrySkill", "LoadAll", "OK", "loaded %d skills total", total);
    /* 导出索引（供 lingosd registry_list / Python 文件回退统一读取） */
    registry_skill_write_index();
    return total;
}

/* ============================================================
 * 【0.6.0 新增】技能索引导出
 *   背景：index.json 此前无写入者（技能链 4 断点之一）——
 *   lingosd registry_list 与 Python 文件回退都读它，但从未被生成。
 *   本函数合并两个来源导出：
 *     ① 磁盘 registry.json 的 type=4 条目（含 skill_store metadata.definition）
 *     ② 内存注册表的 type=4 条目（含目录加载的技能定义——磁盘没有的补充）
 *   输出格式 = 技能 schema 数组（name/description/risk/parameters），
 *   与 ai_server.py load_skill_schemas_from_file 的期望一致。
 * ============================================================ */

/* 判断 schema 数组中是否已含同名技能 */
static int schema_array_has(const cJSON *arr, const char *name) {
    if (!arr || !name) return 0;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *nm = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0) return 1;
    }
    return 0;
}

/* 由技能定义（definition 或整份 skill JSON）构造 schema 条目 */
static cJSON* skill_schema_from_definition(const char *name, const cJSON *meta) {
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;
    cJSON_AddStringToObject(item, "name", name ? name : "");

    const cJSON *def = NULL;
    if (meta) {
        const cJSON *d = cJSON_GetObjectItem(meta, "definition");
        def = cJSON_IsObject(d) ? d : meta;   /* 兼容两种形态 */
    }
    const cJSON *desc = def ? cJSON_GetObjectItem(def, "description") : NULL;
    const cJSON *risk = def ? cJSON_GetObjectItem(def, "risk") : NULL;
    const cJSON *params = def ? cJSON_GetObjectItem(def, "parameters") : NULL;

    cJSON_AddStringToObject(item, "description",
                            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");
    cJSON_AddStringToObject(item, "risk",
                            (risk && cJSON_IsString(risk)) ? risk->valuestring : "low");
    if (params && cJSON_IsObject(params)) {
        cJSON_AddItemToObject(item, "parameters", cJSON_Duplicate(params, 1));
    } else {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "object");
        cJSON_AddItemToObject(p, "properties", cJSON_CreateObject());
        cJSON_AddItemToObject(item, "parameters", p);
    }
    return item;
}

int registry_skill_write_index(void) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/registry/skills/index.json", root);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return -1;

    /* ① 磁盘 registry.json 的 type=4 条目 */
    char regp[512];
    safe_snprintf(regp, sizeof(regp), "%s/registry/core/registry.json", root);
    FILE *rf = fopen(regp, "r");
    if (rf) {
        fseek(rf, 0, SEEK_END);
        long rlen = ftell(rf);
        fseek(rf, 0, SEEK_SET);
        if (rlen > 0) {
            char *rbuf = malloc((size_t)rlen + 1);
            if (rbuf) {
                size_t rr = fread(rbuf, 1, (size_t)rlen, rf);
                rbuf[rr] = '\0';
                cJSON *regj = cJSON_Parse(rbuf);
                free(rbuf);
                if (regj) {
                    cJSON *entries = cJSON_GetObjectItem(regj, "entries");
                    int n = cJSON_IsArray(entries) ? cJSON_GetArraySize(entries) : 0;
                    for (int i = 0; i < n; i++) {
                        cJSON *e = cJSON_GetArrayItem(entries, i);
                        cJSON *t = cJSON_GetObjectItem(e, "type");
                        cJSON *nm = cJSON_GetObjectItem(e, "name");
                        if (!cJSON_IsNumber(t) || t->valueint != REG_TYPE_SKILL) continue;
                        if (!cJSON_IsString(nm) || schema_array_has(arr, nm->valuestring)) continue;
                        cJSON *item = skill_schema_from_definition(nm->valuestring,
                                                                   cJSON_GetObjectItem(e, "metadata"));
                        if (item) cJSON_AddItemToArray(arr, item);
                    }
                    cJSON_Delete(regj);
                }
            }
        }
        fclose(rf);
    }

    /* ② 内存注册表 type=4 条目（补充磁盘尚未包含的——如刚加载的目录技能） */
    registry_entry_t *list[256];
    int n2 = registry_list(REG_TYPE_SKILL, list, 256);
    for (int i = 0; i < n2; i++) {
        if (schema_array_has(arr, list[i]->name)) continue;
        cJSON *item = skill_schema_from_definition(list[i]->name, (cJSON*)list[i]->metadata);
        if (item) cJSON_AddItemToArray(arr, item);
    }

    /* 原子写入（temp + rename） */
    char tmp[512];
    safe_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char *s = cJSON_Print(arr);
    int count = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    if (!s) return -1;

    FILE *fp = fopen(tmp, "w");
    if (!fp) { free(s); LOG_WARN_T("RegistrySkill", "WriteIndex", "OpenFail", "cannot write %s", tmp); return -1; }
    fprintf(fp, "%s\n", s);
    fclose(fp);
    free(s);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        LOG_WARN_T("RegistrySkill", "WriteIndex", "RenameFail", "rename to %s failed", path);
        return -1;
    }
    LOG_INFO_T("RegistrySkill", "WriteIndex", "OK", "index.json exported: %d skills", count);
    return count;
}

/**
 * @brief 获取技能定义（返回 cJSON 指针，调用者不应释放）
 */
cJSON* registry_skill_get_definition(const char *skill_id) {
    const registry_entry_t *entry = registry_get(skill_id);
    if (!entry) return NULL;
    return (cJSON*)entry->metadata;
}