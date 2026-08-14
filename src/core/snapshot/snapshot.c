/**
 * @file    snapshot.c
 * @brief   系统快照实现
 * @version LN-B-4.2.0.0
 */

#include "snapshot.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "version.h"
#include "uart.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#define SNAPSHOT_DIR "/snapshots"
#define MAX_SNAPSHOTS 32

/* ============================================================
 * 内部辅助
 * ============================================================ */

const char* snapshot_get_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, SNAPSHOT_DIR);
    }
    return path;
}

static int ensure_snapshot_dir(void) {
    const char *dir = snapshot_get_dir();
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("Snapshot", "EnsureDir", "Fail", "cannot create %s: %s", dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static char* generate_id(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char *buf = malloc(64);
    if (!buf) return NULL;
    strftime(buf, 64, "snapshot_%Y%m%d_%H%M%S", tm);
    return buf;
}

static int create_snapshot_internal(const char *id, const char *name, const char *desc) {
    const char *root = lingos_data_root();
    const char *snap_dir = snapshot_get_dir();

    char snap_path[512];
    safe_snprintf(snap_path, sizeof(snap_path), "%s/%s", snap_dir, id);

    if (mkdir(snap_path, 0755) != 0) {
        LOG_ERROR_T("Snapshot", "Create", "MkdirFail", "cannot create %s", snap_path);
        return -1;
    }

    /* 复制关键目录 */
    const char *dirs_to_copy[] = {"/system/config", "/data/ai_memory", "/apps", "/Ensystem", NULL};
    for (int i = 0; dirs_to_copy[i]; i++) {
        char src[512];
        char dst[512];
        safe_snprintf(src, sizeof(src), "%s%s", root, dirs_to_copy[i]);
        safe_snprintf(dst, sizeof(dst), "%s%s", snap_path, dirs_to_copy[i]);

        if (access(src, F_OK) == 0) {
            char cmd[1024];
            safe_snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s' 2>/dev/null", src, dst);
            system(cmd);
            LOG_DEBUG_T("Snapshot", "Create", "Copy", "copied %s to %s", dirs_to_copy[i], dst);
        }
    }

    /* 写入元数据 */
    char meta_path[512];
    safe_snprintf(meta_path, sizeof(meta_path), "%s/snapshot.json", snap_path);
    FILE *fp = fopen(meta_path, "w");
    if (!fp) {
        LOG_ERROR_T("Snapshot", "Create", "MetaFail", "cannot write meta to %s", meta_path);
        char cmd[512];
        safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", snap_path);
        system(cmd);
        return -1;
    }

    int64_t size = 0;
    char size_cmd[512];
    safe_snprintf(size_cmd, sizeof(size_cmd), "du -sb '%s' | cut -f1", snap_path);
    FILE *size_fp = popen(size_cmd, "r");
    if (size_fp) {
        fscanf(size_fp, "%lld", (long long*)&size);
        pclose(size_fp);
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"id\": \"%s\",\n", id);
    fprintf(fp, "  \"name\": \"%s\",\n", name && name[0] ? name : id);
    fprintf(fp, "  \"description\": \"%s\",\n", desc ? desc : "");
    fprintf(fp, "  \"created_at\": %lld,\n", (long long)time(NULL));
    fprintf(fp, "  \"size_bytes\": %lld,\n", (long long)size);
    fprintf(fp, "  \"version\": \"%s\"\n", version_get());
    fprintf(fp, "}\n");
    fclose(fp);

    LOG_INFO_T("Snapshot", "Create", "OK", "snapshot %s created", id);
    return 0;
}

static snapshot_info_t* get_snapshot_info(const char *id) {
    const char *snap_dir = snapshot_get_dir();
    char meta_path[512];
    safe_snprintf(meta_path, sizeof(meta_path), "%s/%s/snapshot.json", snap_dir, id);

    FILE *fp = fopen(meta_path, "r");
    if (!fp) return NULL;

    snapshot_info_t *info = malloc(sizeof(snapshot_info_t));
    if (!info) { fclose(fp); return NULL; }
    memset(info, 0, sizeof(snapshot_info_t));

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"id\"")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                char *q = p;
                while (*q && *q != '\"') q++;
                int len = q - p;
                if (len > 0 && len < 64) {
                    memcpy(info->id, p, len);
                    info->id[len] = '\0';
                }
            }
        } else if (strstr(line, "\"name\"")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                char *q = p;
                while (*q && *q != '\"') q++;
                int len = q - p;
                if (len > 0 && len < 128) {
                    memcpy(info->name, p, len);
                    info->name[len] = '\0';
                }
            }
        } else if (strstr(line, "\"created_at\"")) {
            char *p = strchr(line, ':');
            if (p) {
                info->created_at = atoll(p + 1);
            }
        } else if (strstr(line, "\"size_bytes\"")) {
            char *p = strchr(line, ':');
            if (p) {
                info->size_bytes = atoll(p + 1);
            }
        } else if (strstr(line, "\"version\"")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                char *q = p;
                while (*q && *q != '\"') q++;
                int len = q - p;
                if (len > 0 && len < 32) {
                    memcpy(info->version, p, len);
                    info->version[len] = '\0';
                }
            }
        }
    }
    fclose(fp);

    if (info->id[0] == '\0') {
        free(info);
        return NULL;
    }
    return info;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int snapshot_init(void) {
    LOG_INFO_T("Snapshot", "Init", "Enter", "initializing snapshot system");
    if (ensure_snapshot_dir() != 0) {
        LOG_ERROR_T("Snapshot", "Init", "DirFail", "failed to create snapshot directory");
        return -1;
    }
    LOG_INFO_T("Snapshot", "Init", "OK", "snapshot system ready");
    return 0;
}

int snapshot_create(const char *name, const char *description, char *out_id, size_t out_id_len) {
    LOG_INFO_T("Snapshot", "Create", "Enter", "name='%s', desc='%s'",
               name ? name : "(null)", description ? description : "(null)");

    if (ensure_snapshot_dir() != 0) return -1;

    char *id = generate_id();
    if (!id) {
        LOG_ERROR_T("Snapshot", "Create", "IdFail", "failed to generate ID");
        return -1;
    }

    if (create_snapshot_internal(id, name, description) != 0) {
        free(id);
        return -1;
    }

    if (out_id && out_id_len > 0) {
        safe_strncpy(out_id, id, out_id_len);
    }
    free(id);
    return 0;
}

int snapshot_restore(const char *id) {
    LOG_INFO_T("Snapshot", "Restore", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("Snapshot", "Restore", "Invalid", "id is NULL or empty");
        return -1;
    }

    const char *snap_dir = snapshot_get_dir();
    char snap_path[512];
    safe_snprintf(snap_path, sizeof(snap_path), "%s/%s", snap_dir, id);

    if (access(snap_path, F_OK) != 0) {
        LOG_ERROR_T("Snapshot", "Restore", "NotFound", "snapshot %s not found", id);
        return -1;
    }

    const char *root = lingos_data_root();

    uart_puts(COLOR_YELLOW);
    uart_puts(tr("WARNING: Restoring snapshot will overwrite current system data.\n",
                 "警告：恢复快照将覆盖当前系统数据。\n"));
    uart_puts(tr("Are you sure? (y/N): ", "确定？(y/N): "));
    uart_puts(COLOR_RESET);

    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    if (c != 'y' && c != 'Y') {
        LOG_INFO_T("Snapshot", "Restore", "Cancelled", "user cancelled restore");
        return -1;
    }

    /* 先清理目标目录（保留关键文件） */
    const char *dirs_to_restore[] = {"/system/config", "/data/ai_memory", "/apps", "/Ensystem", NULL};
    for (int i = 0; dirs_to_restore[i]; i++) {
        char dst[512];
        char cmd[1024];
        safe_snprintf(dst, sizeof(dst), "%s%s", root, dirs_to_restore[i]);
        safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null", dst);
        system(cmd);
    }

    /* 从快照恢复 */
    for (int i = 0; dirs_to_restore[i]; i++) {
        char src[512];
        char dst[512];
        char cmd[1024];
        safe_snprintf(src, sizeof(src), "%s%s", snap_path, dirs_to_restore[i]);
        safe_snprintf(dst, sizeof(dst), "%s%s", root, dirs_to_restore[i]);

        if (access(src, F_OK) == 0) {
            safe_snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s' 2>/dev/null", src, dst);
            system(cmd);
            LOG_DEBUG_T("Snapshot", "Restore", "Copy", "restored %s", dirs_to_restore[i]);
        }
    }

    LOG_INFO_T("Snapshot", "Restore", "OK", "snapshot %s restored", id);
    uart_puts(tr("Snapshot restored. Please restart LING OS to apply changes.\n",
                 "快照已恢复。请重启 LING OS 使更改生效。\n"));
    return 0;
}

int snapshot_delete(const char *id) {
    LOG_INFO_T("Snapshot", "Delete", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("Snapshot", "Delete", "Invalid", "id is NULL or empty");
        return -1;
    }

    const char *snap_dir = snapshot_get_dir();
    char snap_path[512];
    safe_snprintf(snap_path, sizeof(snap_path), "%s/%s", snap_dir, id);

    if (access(snap_path, F_OK) != 0) {
        LOG_ERROR_T("Snapshot", "Delete", "NotFound", "snapshot %s not found", id);
        return -1;
    }

    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", snap_path);
    system(cmd);

    LOG_INFO_T("Snapshot", "Delete", "OK", "snapshot %s deleted", id);
    return 0;
}

int snapshot_list(snapshot_info_t *out, int max_count) {
    LOG_DEBUG_T("Snapshot", "List", "Enter", "max_count=%d", max_count);

    const char *snap_dir = snapshot_get_dir();
    DIR *d = opendir(snap_dir);
    if (!d) {
        LOG_DEBUG_T("Snapshot", "List", "NoDir", "snapshot directory not found");
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_count) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, "snapshot_", 9) != 0) continue;

        snapshot_info_t *info = get_snapshot_info(entry->d_name);
        if (info) {
            memcpy(&out[count], info, sizeof(snapshot_info_t));
            free(info);
            count++;
        }
    }
    closedir(d);

    LOG_DEBUG_T("Snapshot", "List", "OK", "found %d snapshots", count);
    return count;
}

int snapshot_diff(const char *id, char *out, size_t out_len) {
    LOG_INFO_T("Snapshot", "Diff", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id || !out || out_len == 0) {
        LOG_ERROR_T("Snapshot", "Diff", "Invalid", "id=%p, out=%p, out_len=%zu",
                    (void*)id, (void*)out, out_len);
        return -1;
    }

    const char *snap_dir = snapshot_get_dir();
    char snap_path[512];
    safe_snprintf(snap_path, sizeof(snap_path), "%s/%s", snap_dir, id);

    if (access(snap_path, F_OK) != 0) {
        LOG_ERROR_T("Snapshot", "Diff", "NotFound", "snapshot %s not found", id);
        safe_snprintf(out, out_len, "Snapshot not found");
        return -1;
    }

    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "diff -rq '%s/system/config' /LINGOS/system/config 2>/dev/null | head -50",
                  snap_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        safe_snprintf(out, out_len, "Failed to run diff");
        return -1;
    }

    size_t pos = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && pos < out_len - 1) {
        pos += safe_snprintf(out + pos, out_len - pos, "%s", line);
    }
    pclose(fp);

    if (pos == 0) {
        safe_snprintf(out, out_len, "No differences found");
    }

    LOG_DEBUG_T("Snapshot", "Diff", "OK", "diff output length=%zu", pos);
    return 0;
}

void snapshot_cleanup(void) {
    LOG_INFO_T("Snapshot", "Cleanup", "Enter", "cleaning up snapshot system");
    /* 目前没有需要清理的资源 */
    LOG_INFO_T("Snapshot", "Cleanup", "OK", "snapshot system cleaned up");
}