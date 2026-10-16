/**
 * @file    src/api/skill_catalog.c
 * @brief   API 侧真实技能目录（禁止假数据）
 * @version LN-0.8.0
 *
 * 数据源（与 src/shell/skill_store.c 的两级呈现同口径，均为真实文件）：
 *   ① $ROOT/registry/skills/index.json       —— registry 导出的技能索引
 *   ② $ROOT/registry/core/registry.json      —— type=4 技能条目（metadata.definition）
 *   ③ $ROOT/skills/market/<name>/skill.json  —— 本地技能市场
 *   ④ $ROOT/skills/enabled/<name>/           —— 已启用技能目录
 * 分类：技能自声明 category 优先，否则退回真实来源标签（index/registry/market/enabled）。
 */

#include "skill_catalog.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/cJSON/cJSON.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

#define CAT_SKILLS_INDEX "/registry/skills/index.json"
#define CAT_SKILLS_MARKET "/skills/market"
#define CAT_SKILLS_ENABLED "/skills/enabled"
#define CAT_REGISTRY_FILE "/registry/core/registry.json"
#define CAT_MAX_BYTES (16 * 1024 * 1024)

static void cat_path(char *buf, size_t size, const char *sub) {
    safe_snprintf(buf, size, "%s%s", lingos_data_root(), sub);
}

static char* cat_read_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0 || len > CAT_MAX_BYTES) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rl = fread(buf, 1, (size_t)len, fp);
    buf[rl] = '\0';
    fclose(fp);
    return buf;
}

static const char* cat_field(const cJSON *obj, const char *key) {
    if (!obj || !key) return NULL;
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static int cat_has(const cJSON *arr, const char *name) {
    if (!arr || !name) return 0;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        const char *nm = cat_field(cJSON_GetArrayItem(arr, i), "name");
        if (nm && strcmp(nm, name) == 0) return 1;
    }
    return 0;
}

static void cat_add(cJSON *arr, const char *name, const char *desc,
                    const char *risk, const char *category, const char *source) {
    if (!arr || !name || !*name || cat_has(arr, name)) return;
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "description", desc ? desc : "");
    cJSON_AddStringToObject(item, "risk", (risk && *risk) ? risk : "unknown");
    cJSON_AddStringToObject(item, "category",
                            (category && *category) ? category : (source ? source : "unknown"));
    cJSON_AddStringToObject(item, "source", source ? source : "unknown");
    cJSON_AddItemToArray(arr, item);
}

/* ① 技能索引 */
static void cat_load_index(cJSON *arr) {
    char path[512];
    cat_path(path, sizeof(path), CAT_SKILLS_INDEX);
    char *txt = cat_read_file(path);
    if (!txt) return;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    if (!j) return;
    if (cJSON_IsArray(j)) {
        int n = cJSON_GetArraySize(j);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(j, i);
            cat_add(arr, cat_field(it, "name"), cat_field(it, "description"),
                    cat_field(it, "risk"), cat_field(it, "category"), "index");
        }
    }
    cJSON_Delete(j);
}

/* ② registry.json 的 type=4 条目 */
static void cat_load_registry(cJSON *arr) {
    char path[512];
    cat_path(path, sizeof(path), CAT_REGISTRY_FILE);
    char *txt = cat_read_file(path);
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return;
    cJSON *entries = cJSON_GetObjectItem(root, "entries");
    int n = cJSON_GetArraySize(entries);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        const cJSON *t = cJSON_GetObjectItem(e, "type");
        if (!cJSON_IsNumber(t) || t->valueint != 4) continue;   /* REG_TYPE_SKILL */
        cJSON *meta = cJSON_GetObjectItem(e, "metadata");
        cJSON *def = meta ? cJSON_GetObjectItem(meta, "definition") : NULL;
        const char *cat = cat_field(def, "category");
        if (!cat) cat = cat_field(meta, "category");
        cat_add(arr, cat_field(e, "name"), cat_field(def, "description"),
                cat_field(def, "risk"), cat, "registry");
    }
    cJSON_Delete(root);
}

/* ③ 技能市场 */
static void cat_load_market(cJSON *arr) {
    char dir[512];
    cat_path(dir, sizeof(dir), CAT_SKILLS_MARKET);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sj[640];
        safe_snprintf(sj, sizeof(sj), "%s/%s/skill.json", dir, ent->d_name);
        char *txt = cat_read_file(sj);
        if (!txt) { cat_add(arr, ent->d_name, NULL, NULL, NULL, "market"); continue; }
        cJSON *j = cJSON_Parse(txt);
        free(txt);
        if (!j) { cat_add(arr, ent->d_name, NULL, NULL, NULL, "market"); continue; }
        const char *name = cat_field(j, "name");
        if (!name || !*name) name = ent->d_name;
        cJSON *meta = cJSON_GetObjectItem(j, "metadata");
        cJSON *def = meta ? cJSON_GetObjectItem(meta, "definition") : NULL;
        const char *desc = cat_field(def, "description");
        if (!desc) desc = cat_field(j, "description");
        const char *risk = cat_field(j, "risk");
        if (!risk) risk = cat_field(def, "risk");
        const char *cat = cat_field(j, "category");
        if (!cat) cat = cat_field(def, "category");
        cat_add(arr, name, desc, risk, cat, "market");
        cJSON_Delete(j);
    }
    closedir(d);
}

/* ④ 已启用技能目录 */
static void cat_load_enabled(cJSON *arr) {
    char dir[512];
    cat_path(dir, sizeof(dir), CAT_SKILLS_ENABLED);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        cat_add(arr, ent->d_name, NULL, NULL, NULL, "enabled");
    }
    closedir(d);
}

/* 合并四个真实来源（去重） */
static cJSON* cat_build(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cat_load_index(arr);
    cat_load_registry(arr);
    cat_load_market(arr);
    cat_load_enabled(arr);
    return arr;
}

static int cat_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* 把 cJSON 数组序列化进调用者缓冲（不足则拒绝，不输出截断 JSON） */
static int cat_emit(cJSON *arr, char *out, size_t out_size) {
    char *s = cJSON_PrintUnformatted(arr);
    if (!s) return -1;
    size_t len = strlen(s);
    if (len + 1 > out_size) {
        LOG_WARN_T("SkillCatalog", "Emit", "TooSmall", "need=%zu have=%zu", len + 1, out_size);
        free(s);
        return -1;
    }
    memcpy(out, s, len + 1);
    free(s);
    return 0;
}

int skill_catalog_json(char *out, size_t out_size, const char *category, const char *keyword) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    cJSON *all = cat_build();
    if (!all) return -1;
    cJSON *res = cJSON_CreateArray();
    if (!res) { cJSON_Delete(all); return -1; }

    int n = cJSON_GetArraySize(all);
    int count = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(all, i);
        const char *cat = cat_field(it, "category");
        const char *src = cat_field(it, "source");
        if (category && *category) {
            if (!((cat && strcmp(cat, category) == 0) || (src && strcmp(src, category) == 0))) continue;
        }
        if (keyword && *keyword) {
            if (!cat_contains_ci(cat_field(it, "name"), keyword) &&
                !cat_contains_ci(cat_field(it, "description"), keyword) &&
                !cat_contains_ci(cat, keyword)) continue;
        }
        cJSON *dup = cJSON_Duplicate(it, 1);
        if (dup) { cJSON_AddItemToArray(res, dup); count++; }
    }
    if (cat_emit(res, out, out_size) != 0) {
        cJSON_Delete(res);
        cJSON_Delete(all);
        out[0] = '\0';
        return -1;
    }
    cJSON_Delete(res);
    cJSON_Delete(all);
    return count;
}

int skill_catalog_categories_json(char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    cJSON *all = cat_build();
    if (!all) return -1;

    int n = cJSON_GetArraySize(all);
    /* 小规模：直接构造不重复分类列表 */
    char seen[64][128];
    int seen_n = 0;
    cJSON *res = cJSON_CreateArray();
    if (!res) { cJSON_Delete(all); return -1; }

    for (int i = 0; i < n; i++) {
        const char *cat = cat_field(cJSON_GetArrayItem(all, i), "category");
        if (!cat || !*cat) continue;
        int dup = 0;
        for (int k = 0; k < seen_n; k++) {
            if (strcmp(seen[k], cat) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        if (seen_n < 64) safe_strncpy(seen[seen_n], cat, sizeof(seen[0]));
        seen_n++;
        int cnt = 0;
        for (int j = 0; j < n; j++) {
            const char *c2 = cat_field(cJSON_GetArrayItem(all, j), "category");
            if (c2 && strcmp(c2, cat) == 0) cnt++;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "category", cat);
        cJSON_AddNumberToObject(item, "count", cnt);
        cJSON_AddItemToArray(res, item);
    }
    int rc = (cat_emit(res, out, out_size) == 0) ? seen_n : -1;
    if (rc < 0) out[0] = '\0';
    cJSON_Delete(res);
    cJSON_Delete(all);
    return rc;
}

int skill_catalog_total(void) {
    cJSON *all = cat_build();
    if (!all) return 0;
    int n = cJSON_GetArraySize(all);
    cJSON_Delete(all);
    return n;
}
