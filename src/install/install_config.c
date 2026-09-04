/**
 * @file    src/install/install_config.c
 * @brief   镜像源配置实现
 * @version LN-0.4.3
 * @par     核心协议：C1, CM
 */

#include "install_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONFIG_FILE "/system/config/install.conf"

/* ============================================================
 * 获取配置文件路径
 * ============================================================ */
static void get_config_path(char *path, size_t size) {
    const char *root = lingos_data_root();
    safe_snprintf(path, size, "%s%s", root, CONFIG_FILE);
}

/* ============================================================
 * 获取默认镜像源
 * ============================================================ */
void install_config_get_defaults(char *apt_mirror, char *pypi_mirror) {
    if (apt_mirror) {
        safe_strncpy(apt_mirror, "http://archive.ubuntu.com/ubuntu", 256);
    }
    if (pypi_mirror) {
        safe_strncpy(pypi_mirror, "https://pypi.tuna.tsinghua.edu.cn/simple", 256);
    }
}

/* ============================================================
 * 加载镜像源配置
 * ============================================================ */
int install_config_load_mirrors(char *apt_mirror, char *pypi_mirror) {
    char path[512];
    get_config_path(path, sizeof(path));

    if (access(path, F_OK) != 0) {
        LOG_DEBUG_T("InstallConfig", "Load", "NotFound", "no config file, using defaults");
        install_config_get_defaults(apt_mirror, pypi_mirror);
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        install_config_get_defaults(apt_mirror, pypi_mirror);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[256];
        if (sscanf(line, "%63[^=]=%255s", key, val) == 2) {
            if (strcmp(key, "apt_mirror") == 0 && apt_mirror) {
                safe_strncpy(apt_mirror, val, 256);
            } else if (strcmp(key, "pypi_mirror") == 0 && pypi_mirror) {
                safe_strncpy(pypi_mirror, val, 256);
            }
        }
    }
    fclose(fp);

    LOG_DEBUG_T("InstallConfig", "Load", "OK", "apt_mirror=%s, pypi_mirror=%s",
                apt_mirror ? apt_mirror : "(null)",
                pypi_mirror ? pypi_mirror : "(null)");
    return 0;
}

/* ============================================================
 * 保存镜像源配置
 * ============================================================ */
int install_config_save_mirrors(const char *apt_mirror, const char *pypi_mirror) {
    char path[512];
    get_config_path(path, sizeof(path));

    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("InstallConfig", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "# LING OS Install Configuration\n");
    fprintf(fp, "# Auto-generated\n\n");

    if (apt_mirror) {
        fprintf(fp, "apt_mirror = %s\n", apt_mirror);
    }
    if (pypi_mirror) {
        fprintf(fp, "pypi_mirror = %s\n", pypi_mirror);
    }

    fclose(fp);
    LOG_INFO_T("InstallConfig", "Save", "OK", "config saved to %s", path);
    return 0;
}