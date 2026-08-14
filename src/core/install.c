/**
 * @file    src/core/install.c
 * @brief   首次安装检测与标记
 * @version LN-B-5.1.2.6-rc
 * @changes 增强 installed.state 创建逻辑，确保持久化。
 */

#include "install.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * 检查并运行首次安装流程
 * ============================================================ */
int system_install_check_and_run(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/Ensystem/installed.state", root);

    /* 检查是否已安装 */
    if (access(state_path, F_OK) == 0) {
        LOG_DEBUG_T("Install", "Check", "OK", "System already installed");
        return 0;
    }

    LOG_INFO_T("Install", "Check", "First", "First-time installation detected");

    /* 确保 Ensystem 目录存在 */
    char dir_path[512];
    safe_snprintf(dir_path, sizeof(dir_path), "%s/Ensystem", root);
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR_T("Install", "Dir", "MkdirFail", "mkdir %s failed: %s", dir_path, strerror(errno));
        return -1;
    }

    /* 创建 installed.state 标记 */
    FILE *fp = fopen(state_path, "w");
    if (!fp) {
        LOG_ERROR_T("Install", "State", "CreateFail", "failed to create %s: %s", state_path, strerror(errno));
        return -1;
    }

    fprintf(fp, "installed\n");
    fprintf(fp, "timestamp=%ld\n", (long)time(NULL));
    fclose(fp);

    LOG_INFO_T("Install", "State", "OK", "installed.state created at %s", state_path);

    /* 确保 state.json 也存在（标记为未配置状态） */
    char config_state_path[512];
    safe_snprintf(config_state_path, sizeof(config_state_path), "%s/system/config/state.json", root);
    if (access(config_state_path, F_OK) != 0) {
        char config_dir[512];
        safe_snprintf(config_dir, sizeof(config_dir), "%s/system/config", root);
        mkdir(config_dir, 0755);

        FILE *cfp = fopen(config_state_path, "w");
        if (cfp) {
            fprintf(cfp, "{\n");
            fprintf(cfp, "  \"system_configured\": false,\n");
            fprintf(cfp, "  \"last_config_time\": null\n");
            fprintf(cfp, "}\n");
            fclose(cfp);
            LOG_DEBUG_T("Install", "State", "ConfigState", "state.json created (not configured)");
        }
    }

    return 1;
}

/* ============================================================
 * 检查是否已安装
 * ============================================================ */
int system_is_installed(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/Ensystem/installed.state", root);
    return (access(state_path, F_OK) == 0) ? 1 : 0;
}

/* ============================================================
 * 标记为已安装
 * ============================================================ */
int system_mark_installed(void) {
    const char *root = lingos_data_root();
    char state_path[512];
    safe_snprintf(state_path, sizeof(state_path), "%s/Ensystem/installed.state", root);

    char dir_path[512];
    safe_snprintf(dir_path, sizeof(dir_path), "%s/Ensystem", root);
    mkdir(dir_path, 0755);

    FILE *fp = fopen(state_path, "w");
    if (!fp) {
        LOG_ERROR_T("Install", "Mark", "WriteFail", "failed to write %s", state_path);
        return -1;
    }

    fprintf(fp, "installed\n");
    fprintf(fp, "timestamp=%ld\n", (long)time(NULL));
    fclose(fp);
    LOG_INFO_T("Install", "Mark", "OK", "marked as installed");
    return 0;
}