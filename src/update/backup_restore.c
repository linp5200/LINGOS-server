/**
 * @file    backup_restore.c
 * @brief   备份与回滚
 * @version 2.0.0.0
 */

#include "backup_restore.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

static const char *test_root_override = NULL;

void backup_restore_set_test_root(const char *test_root) {
    test_root_override = test_root;
}

static const char* get_root(void) {
    if (test_root_override) return test_root_override;
    return lingos_data_root();
}

static char last_backup_path[1024] = {0};

const char* backup_lingos(void) {
    const char *root = get_root();
    if (!root) {
        LOG_ERROR_T("Backup", "GetRoot", "Fail", "lingos_data_root returned NULL");
        return NULL;
    }

    char backup_dir[1024];
    snprintf(backup_dir, sizeof(backup_dir), "%s/backups", root);
    if (access(backup_dir, F_OK) != 0) {
        if (mkdir(backup_dir, 0755) != 0 && errno != EEXIST) {
            LOG_ERROR_T("Backup", "Mkdir", "Fail", "cannot create %s: %s", backup_dir, strerror(errno));
            return NULL;
        }
    }

    time_t t = time(NULL);
    snprintf(last_backup_path, sizeof(last_backup_path), "%s/prev_%lld", backup_dir, (long long)t);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cp -a '%s/.' '%s' 2>/dev/null", root, last_backup_path);
    LOG_DEBUG_T("Backup", "Cmd", "Exec", "command: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        LOG_WARN_T("Backup", "Copy", "Partial", "system returned %d, but continuing", ret);
    }

    char test_file[1024];
    snprintf(test_file, sizeof(test_file), "%s/version", last_backup_path);
    if (access(test_file, F_OK) != 0) {
        LOG_WARN_T("Backup", "Verify", "Empty", "backup may be empty, check manually");
    }

    LOG_INFO_T("Backup", "OK", "Path", "backup saved to %s", last_backup_path);
    return last_backup_path;
}

int restore_lingos(void) {
    if (last_backup_path[0] == '\0') {
        LOG_ERROR_T("Restore", "NoBackup", "Fail", "no backup available");
        return -1;
    }

    const char *root = get_root();
    if (!root) {
        LOG_ERROR_T("Restore", "GetRoot", "Fail", "lingos_data_root returned NULL");
        return -1;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* 2>/dev/null", root);
    LOG_DEBUG_T("Restore", "Clean", "Exec", "command: %s", cmd);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "cp -a '%s/.' '%s/' 2>/dev/null", last_backup_path, root);
    int ret = system(cmd);
    if (ret != 0) {
        LOG_WARN_T("Restore", "Copy", "Partial", "system returned %d, but continuing", ret);
    }

    char ensys[1024];
    snprintf(ensys, sizeof(ensys), "%s/Ensystem", root);
    if (access(ensys, F_OK) != 0) {
        if (mkdir(ensys, 0755) != 0) {
            LOG_WARN_T("Restore", "Mkdir", "Ensystem", "cannot create %s", ensys);
        }
    }

    LOG_INFO_T("Restore", "OK", "Done", "restored from %s", last_backup_path);
    return 0;
}