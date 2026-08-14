/**
 * @file    lapt_parser.c
 * @brief   .lapt 包解析（tar.gz + manifest.json）
 * @version 2.0.0.0
 */

#include "lapt_parser.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEMP_DIR_TEMPLATE "/tmp/lapt_install_XXXXXX"

int lapt_install(const char *package_path) {
    if (!package_path || access(package_path, F_OK) != 0) {
        LOG_ERROR_T("LaptParser", "Install", "NoPackage", "%s", package_path);
        return -1;
    }

    /* 创建临时目录 */
    char temp_dir[] = TEMP_DIR_TEMPLATE;
    if (!mkdtemp(temp_dir)) {
        LOG_ERROR_T("LaptParser", "Install", "MkdtempFail", "mkdtemp failed");
        return -1;
    }

    /* 解压 */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C '%s' 2>/dev/null", package_path, temp_dir);
    if (system(cmd) != 0) {
        LOG_ERROR_T("LaptParser", "Install", "ExtractFail", "tar extraction failed");
        rmdir(temp_dir);
        return -1;
    }

    /* 读取 manifest.json 获取应用名称 */
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", temp_dir);
    FILE *fp = fopen(manifest_path, "r");
    if (!fp) {
        LOG_ERROR_T("LaptParser", "Install", "NoManifest", "manifest.json missing");
        unlink(temp_dir);
        return -1;
    }
    char line[512];
    char app_name[128] = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"name\":")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                int i = 0;
                while (*p && *p != '\"' && *p != '\n' && i < 127) app_name[i++] = *p++;
                break;
            }
        }
    }
    fclose(fp);
    if (app_name[0] == '\0') {
        LOG_ERROR_T("LaptParser", "Install", "NoName", "app name not found in manifest");
        unlink(temp_dir);
        return -1;
    }

    /* 目标目录 */
    const char *root = lingos_data_root();
    char target_dir[1024];
    snprintf(target_dir, sizeof(target_dir), "%s/apps/%s", root, app_name);
    if (access(target_dir, F_OK) == 0) {
        LOG_WARN_T("LaptParser", "Install", "Exists", "app %s already installed", app_name);
        unlink(temp_dir);
        return -1;
    }

    /* 复制到目标目录 */
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", temp_dir, target_dir);
    if (system(cmd) != 0) {
        LOG_ERROR_T("LaptParser", "Install", "CopyFail", "failed to copy to %s", target_dir);
        unlink(temp_dir);
        return -1;
    }

    /* 解析入口点（从 manifest.json 读取 entry） */
    fp = fopen(manifest_path, "r");
    char entry_point[512] = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"entry_point\":")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\"') p++;
                int i = 0;
                while (*p && *p != '\"' && *p != '\n' && i < 511) entry_point[i++] = *p++;
                break;
            }
        }
    }
    fclose(fp);
    if (entry_point[0] == '\0') {
        strcpy(entry_point, "run.sh");  // 默认入口
    }

    /* 写入状态文件 */
    char state_dir[1024];
    snprintf(state_dir, sizeof(state_dir), "%s/state/apps", root);
    mkdir(state_dir, 0755);
    char state_path[1024];
    snprintf(state_path, sizeof(state_path), "%s/%s.json", state_dir, app_name);
    fp = fopen(state_path, "w");
    if (fp) {
        fprintf(fp, "{\"name\":\"%s\",\"entry_point\":\"%s\",\"installed\":\"%lld\"}\n", 
                app_name, entry_point, (long long)time(NULL));
        fclose(fp);
    }

    /* 清理临时目录 */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);

    LOG_INFO_T("LaptParser", "Install", "Success", "app=%s", app_name);
    return 0;
}