#include <dirent.h>
#include <unistd.h>
#include "data_path.h"
/**
 * @file    repo_cmds.c
 * @brief   仓库命令实现（app search, app update, app upgrade）
 * @version 2.0.0.0
 */

#include "repo_cmds.h"
#include "../update/repo_client.h"
#include "../lib/lapt_parser.h"
#include "../common/lang.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void repo_search_command(const char *keyword) {
    if (!keyword || !*keyword) {
        uart_puts(tr("Usage: app search <keyword>\n", "用法：app search <关键词>\n"));
        return;
    }
    char *result = repo_search_app(keyword);
    if (!result) {
        uart_puts(tr("Failed to search repository.\n", "搜索仓库失败。\n"));
        return;
    }
    uart_puts(tr("Search results:\n", "搜索结果：\n"));
    uart_puts(result);
    uart_puts("\n");
    free(result);
}

void repo_update_command(const char *app_name) {
    if (!app_name || !*app_name) {
        uart_puts(tr("Usage: app update <appname>\n", "用法：app update <应用名>\n"));
        return;
    }
    char *latest = repo_get_latest_version(app_name);
    if (!latest) {
        uart_puts(tr("App not found in repository.\n", "仓库中未找到该应用。\n"));
        return;
    }
    /* 检查本地版本 */
    const char *root = lingos_data_root();
    char version_file[1024];
    snprintf(version_file, sizeof(version_file), "%s/apps/%s/version", root, app_name);
    FILE *fp = fopen(version_file, "r");
    char current_version[64] = "0";
    if (fp) {
        fgets(current_version, sizeof(current_version), fp);
        char *nl = strchr(current_version, '\n');
        if (nl) *nl = '\0';
        fclose(fp);
    }
    if (strcmp(current_version, latest) == 0) {
        uart_puts(tr("Already up to date.\n", "已经是最新版本。\n"));
        free(latest);
        return;
    }
    uart_puts(tr("Downloading update...\n", "正在下载更新...\n"));
    char temp_path[] = "/tmp/update_XXXXXX.lapt";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        uart_puts(tr("Failed to create temp file.\n", "创建临时文件失败。\n"));
        free(latest);
        return;
    }
    close(fd);
    if (repo_download_app(app_name, temp_path) != 0) {
        uart_puts(tr("Failed to download update.\n", "下载更新失败。\n"));
        unlink(temp_path);
        free(latest);
        return;
    }
    /* 安装更新 */
    if (lapt_install(temp_path) == 0) {
        uart_puts(tr("Update installed successfully.\n", "更新安装成功。\n"));
    } else {
        uart_puts(tr("Update installation failed.\n", "更新安装失败。\n"));
    }
    unlink(temp_path);
    free(latest);
}

void repo_upgrade_command(const char *app_name) {
    if (app_name && *app_name) {
        repo_update_command(app_name);
    } else {
        /* 升级所有已安装应用：扫描 apps 目录，逐一检查更新 */
        const char *root = lingos_data_root();
        char apps_dir[1024];
        snprintf(apps_dir, sizeof(apps_dir), "%s/apps", root);
        DIR *d = opendir(apps_dir);
        if (!d) {
            uart_puts(tr("No apps installed.\n", "没有已安装的应用。\n"));
            return;
        }
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            repo_update_command(entry->d_name);
        }
        closedir(d);
    }
}