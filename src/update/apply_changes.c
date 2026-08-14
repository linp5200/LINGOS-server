/**
 * @file    apply_changes.c
 * @brief   应用更新包中的文件变更（完整实现）
 * @version 2.2.0.0
 */

#include "apply_changes.h"
#include "log_extra.h"
#include "data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

/* 内部辅助：递归创建目录 */
static int mkdir_p(const char *path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                LOG_ERROR_T("ApplyChanges", "Mkdir", "Fail", "mkdir %s: %s", tmp, strerror(errno));
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR_T("ApplyChanges", "Mkdir", "Fail", "mkdir %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* 内部辅助：复制文件 */
static int copy_file(const char *src, const char *dst) {
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) {
        LOG_ERROR_T("ApplyChanges", "Copy", "OpenSrcFail", "cannot open %s", src);
        return -1;
    }
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fclose(fsrc);
        LOG_ERROR_T("ApplyChanges", "Copy", "OpenDstFail", "cannot open %s", dst);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            fclose(fsrc);
            fclose(fdst);
            LOG_ERROR_T("ApplyChanges", "Copy", "WriteFail", "write error to %s", dst);
            return -1;
        }
    }
    fclose(fsrc);
    fclose(fdst);
    LOG_DEBUG_T("ApplyChanges", "Copy", "OK", "copied %s -> %s", src, dst);
    return 0;
}

/* 内部辅助：递归删除目录或文件 */
static int remove_path(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0; // 不存在视为成功
        LOG_ERROR_T("ApplyChanges", "Remove", "StatFail", "stat %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            LOG_ERROR_T("ApplyChanges", "Remove", "OpendirFail", "cannot open %s", path);
            return -1;
        }
        struct dirent *ent;
        int ret = 0;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (remove_path(child) != 0) {
                ret = -1;
                break;
            }
        }
        closedir(dir);
        if (ret == 0 && rmdir(path) != 0 && errno != ENOENT) {
            LOG_ERROR_T("ApplyChanges", "Remove", "RmdirFail", "rmdir %s: %s", path, strerror(errno));
            return -1;
        }
        LOG_DEBUG_T("ApplyChanges", "Remove", "DirOK", "removed dir %s", path);
        return 0;
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            LOG_ERROR_T("ApplyChanges", "Remove", "UnlinkFail", "unlink %s: %s", path, strerror(errno));
            return -1;
        }
        LOG_DEBUG_T("ApplyChanges", "Remove", "FileOK", "removed file %s", path);
        return 0;
    }
}

/**
 * @brief 应用更新包中的变更
 * @param extract_dir 解压临时目录
 * @param m 清单对象
 * @return 0 成功，-1 失败
 */
int apply_changes(const char *extract_dir, manifest_t *m) {
    if (!extract_dir || !m) {
        LOG_ERROR_T("ApplyChanges", "Apply", "InvalidArgs", "extract_dir or manifest is NULL");
        return -1;
    }

    const char *root = lingos_data_root();
    int total_errors = 0;

    for (int i = 0; i < m->component_count; i++) {
        manifest_component_t *comp = &m->components[i];
        LOG_INFO_T("ApplyChanges", "Component", "Start", "applying component: %s (type=%d)", comp->name, comp->type);
        for (int j = 0; j < comp->change_count; j++) {
            change_entry_t *item = &comp->changes[j];
            if (!item->dest) {
                LOG_WARN_T("ApplyChanges", "Item", "Skip", "empty dest, skipping");
                continue;
            }

            /* 构建完整目标路径 */
            char full_dest[1024];
            snprintf(full_dest, sizeof(full_dest), "%s%s", root, item->dest);

            /* 如果 source 存在，则执行复制（添加/替换） */
            if (item->source) {
                char src_path[1024];
                snprintf(src_path, sizeof(src_path), "%s/%s", extract_dir, item->source);
                if (access(src_path, F_OK) != 0) {
                    LOG_WARN_T("ApplyChanges", "Item", "SrcMissing", "source %s not found, skipping", src_path);
                    continue;
                }
                /* 创建目标目录（若需要） */
                char *last_slash = strrchr(full_dest, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    if (mkdir_p(full_dest) != 0) {
                        LOG_ERROR_T("ApplyChanges", "Item", "MkdirFail", "cannot create dir for %s", full_dest);
                        total_errors++;
                        continue;
                    }
                    *last_slash = '/';
                }
                /* 若目标已存在且需备份，则备份（backup 字段） */
                if (item->backup && access(full_dest, F_OK) == 0) {
                    char backup_path[1024];
                    snprintf(backup_path, sizeof(backup_path), "%s.backup", full_dest);
                    if (rename(full_dest, backup_path) != 0) {
                        LOG_WARN_T("ApplyChanges", "Item", "BackupFail", "cannot backup %s", full_dest);
                    } else {
                        LOG_DEBUG_T("ApplyChanges", "Item", "BackupOK", "backed up %s", full_dest);
                    }
                }
                /* 执行复制 */
                if (copy_file(src_path, full_dest) != 0) {
                    LOG_ERROR_T("ApplyChanges", "Item", "CopyFail", "copy %s -> %s failed", src_path, full_dest);
                    total_errors++;
                } else {
                    LOG_INFO_T("ApplyChanges", "Item", "CopyOK", "copied %s -> %s", src_path, full_dest);
                }
            } else {
                /* source 为空，视为删除操作 */
                if (remove_path(full_dest) != 0) {
                    LOG_ERROR_T("ApplyChanges", "Item", "RemoveFail", "remove %s failed", full_dest);
                    total_errors++;
                } else {
                    LOG_INFO_T("ApplyChanges", "Item", "RemoveOK", "removed %s", full_dest);
                }
            }
        }
    }

    if (total_errors > 0) {
        LOG_ERROR_T("ApplyChanges", "Apply", "PartialFail", "%d errors occurred", total_errors);
        return -1;
    }
    LOG_INFO_T("ApplyChanges", "Apply", "Success", "all changes applied successfully");
    return 0;
}