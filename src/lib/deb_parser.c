/**
 * @file    deb_parser.c
 * @brief   .deb 包解析（基本版，提取 data.tar.* 并安装到应用目录）
 * @version 2.0.0.0
 */

#include "deb_parser.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEMP_DIR_TEMPLATE "/tmp/deb_install_XXXXXX"

int deb_install(const char *package_path) {
    if (!package_path || access(package_path, F_OK) != 0) {
        LOG_ERROR_T("DebParser", "Install", "NoPackage", "%s", package_path);
        return -1;
    }

    /* 创建临时目录 */
    char temp_dir[] = TEMP_DIR_TEMPLATE;
    if (!mkdtemp(temp_dir)) {
        LOG_ERROR_T("DebParser", "Install", "MkdtempFail", "mkdtemp failed");
        return -1;
    }

    /* 解压 deb (ar) */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "ar x '%s' --output='%s' 2>/dev/null", package_path, temp_dir);
    if (system(cmd) != 0) {
        LOG_ERROR_T("DebParser", "Install", "ArExtractFail", "ar extraction failed");
        rmdir(temp_dir);
        return -1;
    }

    /* 查找 data.tar.* 文件 */
    char data_tar[1024] = {0};
    snprintf(cmd, sizeof(cmd), "find '%s' -name 'data.tar.*' -type f | head -1", temp_dir);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(data_tar, sizeof(data_tar), fp)) {
            char *nl = strchr(data_tar, '\n');
            if (nl) *nl = '\0';
        }
        pclose(fp);
    }
    if (data_tar[0] == '\0') {
        LOG_ERROR_T("DebParser", "Install", "NoDataTar", "data.tar.* not found");
        unlink(temp_dir);
        return -1;
    }

    /* 提取应用名称（从 control 文件或包名）*/
    char control_path[1024];
    snprintf(control_path, sizeof(control_path), "%s/control.tar.*", temp_dir);
    char control_dir[1024];
    snprintf(control_dir, sizeof(control_dir), "%s/control_extract", temp_dir);
    mkdir(control_dir, 0755);
    snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s' 2>/dev/null", control_path, control_dir);
    system(cmd);
    char app_name[128] = "deb_app";
    char control_file[1024];
    snprintf(control_file, sizeof(control_file), "%s/control", control_dir);
    fp = fopen(control_file, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Package:", 8) == 0) {
                char *p = line + 8;
                while (*p == ' ') p++;
                int i = 0;
                while (*p && *p != '\n' && *p != '\r' && i < 127) app_name[i++] = *p++;
                break;
            }
        }
        fclose(fp);
    }

    /* 目标目录 */
    const char *root = lingos_data_root();
    char target_dir[1024];
    snprintf(target_dir, sizeof(target_dir), "%s/apps/%s", root, app_name);
    if (access(target_dir, F_OK) == 0) {
        LOG_WARN_T("DebParser", "Install", "Exists", "app %s already installed", app_name);
        unlink(temp_dir);
        return -1;
    }

    /* 创建目标目录并解压 data.tar 到其中 */
    mkdir(target_dir, 0755);
    snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s' 2>/dev/null", data_tar, target_dir);
    if (system(cmd) != 0) {
        LOG_ERROR_T("DebParser", "Install", "DataExtractFail", "data.tar extraction failed");
        rmdir(target_dir);
        unlink(temp_dir);
        return -1;
    }

    /* 写入状态文件（入口点尝试猜测）*/
    char state_dir[1024];
    snprintf(state_dir, sizeof(state_dir), "%s/state/apps", root);
    mkdir(state_dir, 0755);
    char state_path[1024];
    snprintf(state_path, sizeof(state_path), "%s/%s.json", state_dir, app_name);
    fp = fopen(state_path, "w");
    if (fp) {
        /* 猜测入口点：寻找 bin/ 或 usr/bin/ 下的可执行文件 */
        char entry[256] = "";
        char check_path[1024];
        snprintf(check_path, sizeof(check_path), "%s/usr/bin/%s", target_dir, app_name);
        if (access(check_path, X_OK) == 0) {
            snprintf(entry, sizeof(entry), "usr/bin/%s", app_name);
        } else {
            snprintf(check_path, sizeof(check_path), "%s/bin/%s", target_dir, app_name);
            if (access(check_path, X_OK) == 0) {
                snprintf(entry, sizeof(entry), "bin/%s", app_name);
            } else {
                strcpy(entry, "usr/bin/unknown");
            }
        }
        fprintf(fp, "{\"name\":\"%s\",\"entry_point\":\"%s\",\"installed\":\"%lld\"}\n", 
                app_name, entry, (long long)time(NULL));
        fclose(fp);
    }

    /* 清理 */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);

    LOG_INFO_T("DebParser", "Install", "Success", "app=%s", app_name);
    return 0;
}