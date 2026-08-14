#include "fs_layout.h"
#include "log_extra.h"
#include "version.h"
#include "data_path.h"
#include "mode.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static void mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    char *p = NULL;
    size_t len;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len-1] == '/') tmp[len-1] = 0;
    for (p = tmp+1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (access(tmp, F_OK) != 0) mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (access(tmp, F_OK) != 0) mkdir(tmp, mode);
}

void do_create_layout(void) {
    const char *root = lingos_data_root();

    const char *subdirs[] = {
        "/system", "/system/config", "/system/config/custom",
        "/system/modules", "/system/backups",
        "/skills", "/skills/builtin", "/skills/custom", "/skills/store",
        "/data", "/data/ai_memory",
        "/data/ai_memory/ai_smemory", "/data/ai_memory/ai_mmemory", "/data/ai_memory/ai_lmemory",
        "/data/logs", "/data/shared",
        "/Ensystem", "/Ensystem/private", "/Ensystem/baseline", "/Ensystem/backups",
        "/Debug", "/Debug/log",
        "/state", "/state/components",
        "/apps",
        "/bin",
        "/run",
        NULL
    };

    if (access(root, F_OK) != 0) mkdir(root, 0755);
    for (int i = 0; subdirs[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s", root, subdirs[i]);
        mkdir_p(path, 0755);
    }

    char version_path[512];
    snprintf(version_path, sizeof(version_path), "%s/version", root);
    if (access(version_path, F_OK) != 0) {
        FILE *fp = fopen(version_path, "w");
        if (fp) {
            fprintf(fp, "%s\n", LINGOS_VERSION);
            fclose(fp);
        }
    }

    // skills/index.json 现在由 Python 端管理，C 端仅创建空占位文件
    // 【修复】路径对齐 lingosd/ai_server 读取路径：/LINGOS/registry/skills/index.json
    char reg_skills_dir[512];
    snprintf(reg_skills_dir, sizeof(reg_skills_dir), "%s/registry/skills", root);
    if (access(reg_skills_dir, F_OK) != 0) {
        mkdir(reg_skills_dir, 0755);
    }
    char idx_path[512];
    snprintf(idx_path, sizeof(idx_path), "%s/registry/skills/index.json", root);
    if (access(idx_path, F_OK) != 0) {
        FILE *fp = fopen(idx_path, "w");
        if (fp) {
            // 写入空索引，Python 启动时会覆盖
            fprintf(fp, "[]\n");
            fclose(fp);
        }
    }

    char idx_ver_path[512];
    snprintf(idx_ver_path, sizeof(idx_ver_path), "%s/skills/index_version", root);
    if (access(idx_ver_path, F_OK) != 0) {
        FILE *fp = fopen(idx_ver_path, "w");
        if (fp) { fprintf(fp, "2.0\n"); fclose(fp); }
    }

    char reg_path[512];
    snprintf(reg_path, sizeof(reg_path), "%s/data/ai_memory/memory_registry.json", root);
    if (access(reg_path, F_OK) != 0) {
        FILE *fp = fopen(reg_path, "w");
        if (fp) { fprintf(fp, "{\"version\":\"1.0\",\"entries\":[]}\n"); fclose(fp); }
    }

    char mem_ver_path[512];
    snprintf(mem_ver_path, sizeof(mem_ver_path), "%s/data/ai_memory/version", root);
    if (access(mem_ver_path, F_OK) != 0) {
        FILE *fp = fopen(mem_ver_path, "w");
        if (fp) { fprintf(fp, "1.0\n"); fclose(fp); }
    }

    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/Ensystem/state.json", root);
    if (access(state_path, F_OK) != 0) {
        FILE *fp = fopen(state_path, "w");
        if (fp) {
            fprintf(fp, "{\"system_configured\":false,\"last_config_time\":\"\",\"mode\":\"%s\"}\n",
                    lingos_mode_name(lingos_get_mode()));
            fclose(fp);
        }
    }

    char passwd_path[512];
    snprintf(passwd_path, sizeof(passwd_path), "%s/Ensystem/passwd", root);
    if (access(passwd_path, F_OK) != 0) {
        FILE *fp = fopen(passwd_path, "w");
        if (fp) { fprintf(fp, "root:\n"); fclose(fp); }
    }

    char audit_ver_path[512];
    snprintf(audit_ver_path, sizeof(audit_ver_path), "%s/Ensystem/audit_version", root);
    if (access(audit_ver_path, F_OK) != 0) {
        FILE *fp = fopen(audit_ver_path, "w");
        if (fp) { fprintf(fp, "1\n"); fclose(fp); }
    }

    char ai_cfg_ver_path[512];
    snprintf(ai_cfg_ver_path, sizeof(ai_cfg_ver_path), "%s/system/config/ai_config_version", root);
    if (access(ai_cfg_ver_path, F_OK) != 0) {
        FILE *fp = fopen(ai_cfg_ver_path, "w");
        if (fp) { fprintf(fp, "1.0\n"); fclose(fp); }
    }

    char threshold_path[512];
    snprintf(threshold_path, sizeof(threshold_path), "%s/Ensystem/baseline/threshold.conf", root);
    if (access(threshold_path, F_OK) != 0) {
        FILE *fp = fopen(threshold_path, "w");
        if (fp) { fprintf(fp, "50\n"); fclose(fp); }
    }

    char test_log_path[512];
    snprintf(test_log_path, sizeof(test_log_path), "%s/Debug/test.log", root);
    if (access(test_log_path, F_OK) != 0) {
        FILE *fp = fopen(test_log_path, "w");
        if (fp) fclose(fp);
    }

    uart_puts("[FS] Layout initialization complete.\n");
}

void fs_layout_init(void) {
    LOG_INFO_T("FS", "Init", "Start", "Initializing filesystem layout...");
    do_create_layout();
    LOG_INFO_T("FS", "Init", "Done", "Filesystem layout initialized.");
}