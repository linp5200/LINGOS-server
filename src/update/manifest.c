/**
 * @file    manifest.c
 * @brief   更新清单解析（支持修复元数据）
 * @version 2.2.0.0
 */

#include "manifest.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* 简单 JSON 解析（仅提取必要字段） */
static int parse_component_type(const char *type_str) {
    if (!type_str) return COMPONENT_OTHER;
    if (strcmp(type_str, "binary") == 0) return COMPONENT_BINARY;
    if (strcmp(type_str, "config") == 0) return COMPONENT_CONFIG;
    if (strcmp(type_str, "web") == 0) return COMPONENT_WEB;
    return COMPONENT_OTHER;
}

/* 从字符串中提取第一个 JSON 值（辅助） */
static char* extract_json_value(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char *q = p;
    while (*q && *q != '"') q++;
    if (*q != '"') return NULL;
    int len = q - p;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

static int extract_repair_meta(const char *json_buf, repair_meta_t *meta) {
    if (!json_buf || !meta) return -1;
    memset(meta, 0, sizeof(repair_meta_t));

    char *val;
    val = extract_json_value(json_buf, "reason");
    if (val) { strncpy(meta->reason, val, sizeof(meta->reason)-1); free(val); }
    val = extract_json_value(json_buf, "trigger");
    if (val) { strncpy(meta->trigger, val, sizeof(meta->trigger)-1); free(val); }
    val = extract_json_value(json_buf, "fingerprint");
    if (val) { strncpy(meta->fingerprint, val, sizeof(meta->fingerprint)-1); free(val); }
    val = extract_json_value(json_buf, "author");
    if (val) { strncpy(meta->author, val, sizeof(meta->author)-1); free(val); }
    // confidence 是数字
    char *p = strstr(json_buf, "\"confidence\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            meta->confidence = atof(p);
        }
    }
    return 0;
}

int manifest_parse(const char *path, manifest_t *out) {
    if (!path || !out) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR_T("Manifest", "Parse", "OpenFail", "cannot open %s", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    /* 简化解析，只提取 version 和 previous_version */
    memset(out, 0, sizeof(manifest_t));
    char *v = extract_json_value(buf, "version");
    if (v) { out->version = v; }
    char *pv = extract_json_value(buf, "previous_version");
    if (pv) { out->previous_version = pv; }

    /* 检测是否有 web 组件（占位） */
    if (strstr(buf, "\"type\":\"web\"")) {
        out->component_count = 1;
        out->components = malloc(sizeof(manifest_component_t));
        if (out->components) {
            out->components[0].type = COMPONENT_WEB;
            out->components[0].name = strdup("web");
            out->components[0].version = out->version ? strdup(out->version) : strdup("0.0.0");
            out->components[0].changes = NULL;
            out->components[0].change_count = 0;
        }
    }

    if (strstr(buf, "\"requires_reboot\"")) {
        out->requires_reboot = (strstr(buf, "true") != NULL);
    }
    if (strstr(buf, "\"requires_confirm\"")) {
        out->requires_confirm = (strstr(buf, "true") != NULL);
    }

    free(buf);
    return 0;
}

int manifest_parse_with_repair(const char *extract_dir,
                               char *version, size_t vlen,
                               char *source_type, size_t stlen,
                               repair_meta_t *repair_meta) {
    if (!extract_dir || !version || !source_type || !repair_meta) return -1;
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/system.json", extract_dir);
    FILE *fp = fopen(json_path, "r");
    if (!fp) {
        LOG_ERROR_T("Manifest", "ParseWithRepair", "OpenFail", "cannot open %s", json_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    /* 提取 version */
    char *v = extract_json_value(buf, "version");
    if (v) {
        strncpy(version, v, vlen-1);
        version[vlen-1] = '\0';
        free(v);
    } else {
        LOG_ERROR_T("Manifest", "ParseWithRepair", "NoVersion", "version field missing");
        free(buf);
        return -1;
    }

    /* 提取 source_type */
    char *st = extract_json_value(buf, "source_type");
    if (st) {
        strncpy(source_type, st, stlen-1);
        source_type[stlen-1] = '\0';
        free(st);
    } else {
        strcpy(source_type, "binary");
    }

    /* 提取修复元数据（如果存在） */
    if (strstr(buf, "\"repair_meta\"")) {
        char *meta_start = strstr(buf, "\"repair_meta\"");
        if (meta_start) {
            // 找到对应的对象范围，简单提取所有字段
            extract_repair_meta(buf, repair_meta);
        }
    }

    free(buf);
    return 0;
}

void manifest_free(manifest_t *m) {
    if (!m) return;
    free(m->version);
    free(m->previous_version);
    if (m->components) {
        for (int i = 0; i < m->component_count; i++) {
            free(m->components[i].name);
            free(m->components[i].version);
            if (m->components[i].changes) {
                for (int j = 0; j < m->components[i].change_count; j++) {
                    free(m->components[i].changes[j].source);
                    free(m->components[i].changes[j].dest);
                }
                free(m->components[i].changes);
            }
        }
        free(m->components);
    }
    memset(m, 0, sizeof(manifest_t));
}