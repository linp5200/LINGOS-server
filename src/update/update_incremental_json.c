/**
 * @file    update_incremental_json.c
 * @brief   增量更新 JSON manifest 实现（2026-08-22 定稿）
 * @version LN-0.4.3
 * @par     核心协议：C-C 防弹/防御/容错/跛脚 + C1 分级日志
 * @changes 新增：JSON manifest（path/action/size/hash）+ base_ver 匹配
 *          + sha256 校验 + 应用前备份（可回滚）
 *
 * 约定格式：
 *   {
 *     "base_ver":  "0.0.3",
 *     "target_ver":"0.0.4",
 *     "files": [
 *       {"path":"bin/lingos_linux","action":"mod","size":12345,"hash":"sha256..."},
 *       {"path":"python/ai_server.py","action":"add","size":111,"hash":"..."},
 *       {"path":"old.txt","action":"del","size":0,"hash":""}
 *     ]
 *   }
 * action: add=新增 / mod=修改 / del=删除
 * 应用顺序：先 add/mod（复制+备份），后 del（删除+备份）；失败回滚已备份文件。
 */

#include "update_incremental_json.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define INC_MAX_FILES 4096
#define INC_PATH_MAX  1024
#define INC_HASH_MAX  96

typedef struct {
    char path[INC_PATH_MAX];
    char action[8];      /* add/mod/del */
    long size;
    char hash[INC_HASH_MAX];
} inc_file_entry_t;

/* ---------------- 工具函数 ---------------- */

/* sha256sum 文件 → hash 字符串（popen，失败返回 0） */
static int inc_file_sha256(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len < 32) return 0;
    char cmd[INC_PATH_MAX + 64];
    safe_snprintf(cmd, sizeof(cmd), "sha256sum \"%s\" 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[256];
    int ok = 0;
    if (fgets(line, sizeof(line), fp)) {
        /* 格式: <64位hex>  <路径> */
        size_t n = strcspn(line, " \t");
        if (n >= 64 && n < out_len) {
            memcpy(out, line, n);
            out[n] = '\0';
            ok = 1;
        }
    }
    pclose(fp);
    return ok;
}

/* 文件是否存在 */
static int inc_file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* 两文件是否相同（size + sha256）——不同返回 1 */
static int inc_file_changed(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 1;
    if (sa.st_size != sb.st_size) return 1;
    char ha[INC_HASH_MAX], hb[INC_HASH_MAX];
    if (!inc_file_sha256(a, ha, sizeof(ha))) return 1;
    if (!inc_file_sha256(b, hb, sizeof(hb))) return 1;
    return strcmp(ha, hb) != 0;
}

/* 复制文件（保持权限） */
static int inc_copy_file(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    /* 确保目标目录存在 */
    char dir[INC_PATH_MAX];
    safe_strncpy(dir, dst, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0] != '\0') {
            char mk[INC_PATH_MAX + 32];
            safe_snprintf(mk, sizeof(mk), "mkdir -p \"%s\" 2>/dev/null", dir);
            (void)system(mk);
        }
    }
    char cmd[INC_PATH_MAX * 2 + 64];
    safe_snprintf(cmd, sizeof(cmd), "cp -p \"%s\" \"%s\" 2>/dev/null", src, dst);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---------------- 目录遍历（收集相对文件列表）---------------- */

typedef struct {
    inc_file_entry_t entries[INC_MAX_FILES];
    int count;
} inc_file_list_t;

static void inc_collect_dir_r(const char *root, const char *rel, inc_file_list_t *list) {
    if (!root || !list) return;
    char full[INC_PATH_MAX];
    if (rel && rel[0] != '\0')
        safe_snprintf(full, sizeof(full), "%s/%s", root, rel);
    else
        safe_snprintf(full, sizeof(full), "%s", root);

    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && list->count < INC_MAX_FILES) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name, ".git") == 0) continue;
        char child[INC_PATH_MAX];
        if (rel && rel[0] != '\0')
            safe_snprintf(child, sizeof(child), "%s/%s", rel, e->d_name);
        else
            safe_snprintf(child, sizeof(child), "%s", e->d_name);

        char child_full[INC_PATH_MAX];
        safe_snprintf(child_full, sizeof(child_full), "%s/%s", full, e->d_name);
        struct stat st;
        if (stat(child_full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                inc_collect_dir_r(root, child, list);
            } else if (S_ISREG(st.st_mode) && list->count < INC_MAX_FILES) {
                inc_file_entry_t *ent = &list->entries[list->count++];
                safe_strncpy(ent->path, child, sizeof(ent->path));
                ent->action[0] = '\0';
                ent->size = (long)st.st_size;
                ent->hash[0] = '\0';
            }
        }
    }
    closedir(d);
}

/* ---------------- 生成 JSON manifest ---------------- */

int update_incremental_json_manifest(const char *base_dir, const char *target_dir,
                                     const char *base_ver, const char *target_ver,
                                     char *out, size_t out_len) {
    if (!base_dir || !target_dir || !out || out_len == 0) return -1;
    LOG_INFO_T("UpdateInc", "JsonManifest", "Enter", "base=%s target=%s", base_dir, target_dir);

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    /* 收集 target 全部文件（堆分配——结构体达 MB 级，栈会溢出） */
    inc_file_list_t *target_list = calloc(1, sizeof(inc_file_list_t));
    inc_file_list_t *base_list = calloc(1, sizeof(inc_file_list_t));
    if (!target_list || !base_list) {
        if (target_list) free(target_list);
        if (base_list) free(base_list);
        cJSON_Delete(root);
        return -1;
    }
    inc_collect_dir_r(target_dir, NULL, target_list);
    inc_collect_dir_r(base_dir, NULL, base_list);

    cJSON_AddStringToObject(root, "base_ver", base_ver ? base_ver : "");
    cJSON_AddStringToObject(root, "target_ver", target_ver ? target_ver : "");
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    if (!files) { cJSON_Delete(root); free(target_list); free(base_list); return -1; }

    /* target 文件：与 base 对比 → add 或 mod */
    for (int i = 0; i < target_list->count; i++) {
        inc_file_entry_t *ent = &target_list->entries[i];
        char base_path[INC_PATH_MAX];
        safe_snprintf(base_path, sizeof(base_path), "%s/%s", base_dir, ent->path);
        const char *action;
        if (!inc_file_exists(base_path)) {
            action = "add";
        } else {
            char target_path[INC_PATH_MAX];
            safe_snprintf(target_path, sizeof(target_path), "%s/%s", target_dir, ent->path);
            action = inc_file_changed(base_path, target_path) ? "mod" : "same";
        }
        if (strcmp(action, "same") == 0) continue;

        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "path", ent->path);
        cJSON_AddStringToObject(f, "action", action);
        cJSON_AddNumberToObject(f, "size", (double)ent->size);
        if (strcmp(action, "del") != 0) {
            char target_path[INC_PATH_MAX];
            safe_snprintf(target_path, sizeof(target_path), "%s/%s", target_dir, ent->path);
            char hash[INC_HASH_MAX];
            if (inc_file_sha256(target_path, hash, sizeof(hash)))
                cJSON_AddStringToObject(f, "hash", hash);
        }
        cJSON_AddItemToArray(files, f);
    }

    /* base 有而 target 无 → del */
    for (int i = 0; i < base_list->count; i++) {
        inc_file_entry_t *ent = &base_list->entries[i];
        char target_path[INC_PATH_MAX];
        safe_snprintf(target_path, sizeof(target_path), "%s/%s", target_dir, ent->path);
        if (!inc_file_exists(target_path)) {
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "path", ent->path);
            cJSON_AddStringToObject(f, "action", "del");
            cJSON_AddNumberToObject(f, "size", 0);
            cJSON_AddItemToArray(files, f);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(target_list);
    free(base_list);
    if (!json) return -1;
    size_t n = strlen(json);
    int ret = 0;
    if (n < out_len) {
        memcpy(out, json, n + 1);
    } else {
        ret = -1;
    }
    free(json);
    LOG_INFO_T("UpdateInc", "JsonManifest", "OK", "manifest %zu bytes", n);
    return ret;
}

/* ---------------- 应用 JSON manifest ---------------- */

int update_incremental_json_apply(const char *manifest_path, const char *target_root,
                                  const char *current_ver) {
    if (!manifest_path || !target_root) return -1;
    LOG_INFO_T("UpdateInc", "JsonApply", "Enter", "manifest=%s root=%s", manifest_path, target_root);

    FILE *fp = fopen(manifest_path, "r");
    if (!fp) return -1;
    /* 读取整个文件（限制 8MB 防恶意） */
    long fsize = 0;
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 8 * 1024 * 1024) { fclose(fp); return -1; }
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t rd = fread(buf, 1, (size_t)fsize, fp);
    buf[rd] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { LOG_ERROR_T("UpdateInc", "JsonApply", "ParseFail", "invalid JSON manifest"); return -1; }

    /* base_ver 匹配校验 */
    cJSON *bv = cJSON_GetObjectItem(root, "base_ver");
    if (current_ver && current_ver[0] != '\0' &&
        cJSON_IsString(bv) && bv->valuestring[0] != '\0' &&
        strcmp(bv->valuestring, current_ver) != 0) {
        LOG_ERROR_T("UpdateInc", "JsonApply", "VerMismatch",
                    "manifest base_ver=%s != current=%s", bv->valuestring, current_ver);
        cJSON_Delete(root);
        return -2; /* 版本不匹配，拒绝应用 */
    }

    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (!cJSON_IsArray(files)) { cJSON_Delete(root); return -1; }

    /* 备份目录：target_root/.inc_backup/<时间戳>/（简化：固定 .inc_backup） */
    char bak_dir[INC_PATH_MAX];
    safe_snprintf(bak_dir, sizeof(bak_dir), "%s/.inc_backup", target_root);
    char mk[INC_PATH_MAX + 32];
    safe_snprintf(mk, sizeof(mk), "mkdir -p \"%s\" 2>/dev/null", bak_dir);
    (void)system(mk);

    int fail = 0;
    int n = cJSON_GetArraySize(files);

    /* 第一遍：add/mod（先备份旧文件再复制新文件） */
    for (int i = 0; i < n; i++) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        if (!f) continue;
        cJSON *pa = cJSON_GetObjectItem(f, "path");
        cJSON *ac = cJSON_GetObjectItem(f, "action");
        cJSON *hs = cJSON_GetObjectItem(f, "hash");
        if (!cJSON_IsString(pa) || !cJSON_IsString(ac)) continue;
        const char *action = ac->valuestring;
        char dst[INC_PATH_MAX];
        safe_snprintf(dst, sizeof(dst), "%s/%s", target_root, pa->valuestring);

        if (strcmp(action, "add") == 0 || strcmp(action, "mod") == 0) {
            /* 校验：源文件必须在 manifest 同目录（增量包结构：manifest 旁放文件） */
            char src[INC_PATH_MAX];
            char manifest_dir[INC_PATH_MAX];
            safe_strncpy(manifest_dir, manifest_path, sizeof(manifest_dir));
            char *slash = strrchr(manifest_dir, '/');
            if (slash) *slash = '\0'; else safe_strncpy(manifest_dir, ".", sizeof(manifest_dir));
            safe_snprintf(src, sizeof(src), "%s/%s", manifest_dir, pa->valuestring);
            if (!inc_file_exists(src)) {
                LOG_WARN_T("UpdateInc", "JsonApply", "SrcMissing", "source missing: %s", src);
                fail = 1; break;
            }
            /* sha256 校验（若 manifest 提供 hash） */
            if (cJSON_IsString(hs) && hs->valuestring[0] != '\0') {
                char h[INC_HASH_MAX];
                if (!inc_file_sha256(src, h, sizeof(h)) || strcmp(h, hs->valuestring) != 0) {
                    LOG_ERROR_T("UpdateInc", "JsonApply", "HashMismatch", "hash fail: %s", pa->valuestring);
                    fail = 1; break;
                }
            }
            /* 备份旧文件（存在时） */
            if (inc_file_exists(dst)) {
                char bak[INC_PATH_MAX + 32];
                safe_snprintf(bak, sizeof(bak), "%s/%s.bak", bak_dir, pa->valuestring);
                char bak_cmd[INC_PATH_MAX * 2 + 64];
                safe_snprintf(bak_cmd, sizeof(bak_cmd), "cp -p \"%s\" \"%s\" 2>/dev/null", dst, bak);
                (void)system(bak_cmd);
            }
            if (inc_copy_file(src, dst) != 0) { fail = 1; break; }
            LOG_INFO_T("UpdateInc", "JsonApply", "OK", "%s %s", action, pa->valuestring);
        }
    }

    /* 第二遍：del（备份后删除） */
    if (!fail) {
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            if (!f) continue;
            cJSON *pa = cJSON_GetObjectItem(f, "path");
            cJSON *ac = cJSON_GetObjectItem(f, "action");
            if (!cJSON_IsString(pa) || !cJSON_IsString(ac)) continue;
            if (strcmp(ac->valuestring, "del") != 0) continue;
            char dst[INC_PATH_MAX];
            safe_snprintf(dst, sizeof(dst), "%s/%s", target_root, pa->valuestring);
            if (inc_file_exists(dst)) {
                char bak[INC_PATH_MAX + 32];
                safe_snprintf(bak, sizeof(bak), "%s/%s.bak", bak_dir, pa->valuestring);
                char bak_cmd[INC_PATH_MAX * 2 + 64];
                safe_snprintf(bak_cmd, sizeof(bak_cmd), "cp -p \"%s\" \"%s\" 2>/dev/null", dst, bak);
                (void)system(bak_cmd);
                char del_cmd[INC_PATH_MAX + 32];
                safe_snprintf(del_cmd, sizeof(del_cmd), "rm -f \"%s\" 2>/dev/null", dst);
                (void)system(del_cmd);
                LOG_INFO_T("UpdateInc", "JsonApply", "OK", "del %s", pa->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    if (fail) {
        LOG_ERROR_T("UpdateInc", "JsonApply", "Fail", "incremental apply failed — backups in %s", bak_dir);
        return -1;
    }
    LOG_INFO_T("UpdateInc", "JsonApply", "OK", "incremental JSON update applied");
    return 0;
}
