/**
 * @file    app_cmds.c
 * @brief   应用管理器命令实现（install/uninstall/list/run/stop/logs）
 *          B4 增强：依赖解析、沙箱启动
 * @version 2.0.0.0
 */

#include "app_cmds.h"
#include "../lib/lapt_parser.h"
#include "../lib/deb_parser.h"
#include "../lib/pkg_deps.h"
#include "../core/app_runner.h"
#include "../core/app_sandbox.h"
#include "../common/lang.h"
#include "log_extra.h"
#include "uart.h"
#include "../common/data_path.h"
#include "../security/audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define APPS_DIR "/apps"
#define APPS_STATE_DIR "/state/apps"

static const char *get_apps_root(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, APPS_DIR);
    }
    return path;
}

static const char *get_apps_state_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, APPS_STATE_DIR);
    }
    return path;
}

static void list_installed_apps(void) {
    const char *apps_root = get_apps_root();
    DIR *d = opendir(apps_root);
    if (!d) {
        uart_puts(tr("No apps installed.\n", "没有已安装的应用。\n"));
        return;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char app_path[1024];
        snprintf(app_path, sizeof(app_path), "%s/%s", apps_root, entry->d_name);
        struct stat st;
        if (stat(app_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int running = app_is_running(entry->d_name);
        uart_puts("  ");
        uart_puts(entry->d_name);
        uart_puts(" - ");
        uart_puts(running ? tr("running\n", "运行中\n") : tr("stopped\n", "已停止\n"));
        count++;
    }
    closedir(d);
    if (count == 0) {
        uart_puts(tr("No apps installed.\n", "没有已安装的应用。\n"));
    }
}

void app_install_command(const char *package_path) {
    if (!package_path || !*package_path) {
        uart_puts(tr("Usage: app install <path> (支持 .lapt 或 .deb)\n", "用法：app install <路径> (支持 .lapt 或 .deb)\n"));
        return;
    }
    if (access(package_path, F_OK) != 0) {
        uart_puts(tr("Package not found.\n", "安装包未找到。\n"));
        return;
    }
    const char *ext = strrchr(package_path, '.');
    if (!ext) {
        uart_puts(tr("Unknown package format. Use .lapt or .deb\n", "未知包格式。请使用 .lapt 或 .deb\n"));
        return;
    }
    /* 先解析依赖 */
    char *deps = pkg_resolve_deps(package_path);
    if (deps && deps[0] != '\0') {
        uart_puts(tr("Resolving dependencies: ", "解析依赖: "));
        uart_puts(deps);
        uart_puts("\n");
        if (pkg_download_deps(deps) != 0) {
            uart_puts(tr("Failed to download dependencies. Install anyway? (y/N): ", "依赖下载失败。仍继续安装？(y/N): "));
            char c = uart_getc();
            uart_putc(c);
            uart_puts("\n");
            if (c != 'y' && c != 'Y') {
                free(deps);
                return;
            }
        }
    }
    free(deps);
    /* 安装包 */
    if (strcmp(ext, ".lapt") == 0) {
        if (lapt_install(package_path) == 0) {
            uart_puts(tr("App installed successfully.\n", "应用安装成功。\n"));
            audit_log("shell", "app_install", package_path, "", "success", 0, "medium", 1);
        } else {
            uart_puts(tr("App installation failed.\n", "应用安装失败。\n"));
            audit_log("shell", "app_install", package_path, "", "failed", -1, "medium", 1);
        }
    } else if (strcmp(ext, ".deb") == 0) {
        if (deb_install(package_path) == 0) {
            uart_puts(tr("Deb package installed as app.\n", "Deb 包已安装为应用。\n"));
            audit_log("shell", "app_install", package_path, "", "success", 0, "medium", 1);
        } else {
            uart_puts(tr("Deb installation failed.\n", "Deb 安装失败。\n"));
            audit_log("shell", "app_install", package_path, "", "failed", -1, "medium", 1);
        }
    } else {
        uart_puts(tr("Unsupported format. Use .lapt or .deb\n", "不支持的格式。请使用 .lapt 或 .deb\n"));
    }
}

void app_uninstall_command(const char *app_name) {
    if (!app_name || !*app_name) {
        uart_puts(tr("Usage: app uninstall <appname>\n", "用法：app uninstall <应用名>\n"));
        return;
    }
    const char *apps_root = get_apps_root();
    char app_path[1024];
    snprintf(app_path, sizeof(app_path), "%s/%s", apps_root, app_name);
    if (access(app_path, F_OK) != 0) {
        uart_puts(tr("App not found.\n", "应用未找到。\n"));
        return;
    }
    app_stop(app_name);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", app_path);
    if (system(cmd) == 0) {
        uart_puts(tr("App uninstalled.\n", "应用已卸载。\n"));
        audit_log("shell", "app_uninstall", app_name, "", "success", 0, "medium", 1);
    } else {
        uart_puts(tr("Failed to uninstall app.\n", "卸载应用失败。\n"));
        audit_log("shell", "app_uninstall", app_name, "", "failed", -1, "medium", 1);
    }
}

void app_list_command(void) {
    list_installed_apps();
}

void app_run_command(const char *app_name) {
    if (!app_name || !*app_name) {
        uart_puts(tr("Usage: app run <appname> [--sandbox]\n", "用法：app run <应用名> [--sandbox]\n"));
        return;
    }
    int sandbox = 0;
    char name_copy[128];
    strncpy(name_copy, app_name, sizeof(name_copy)-1);
    char *arg = strstr(name_copy, " --sandbox");
    if (arg) {
        *arg = '\0';
        sandbox = 1;
    }
    int ret;
    if (sandbox) {
        ret = app_start_sandboxed(name_copy);
        uart_puts(ret == 0 ? tr("App started in sandbox.\n", "应用已在沙箱中启动。\n") 
                           : tr("Failed to start app in sandbox.\n", "沙箱启动失败。\n"));
    } else {
        ret = app_start(name_copy);
        uart_puts(ret == 0 ? tr("App started.\n", "应用已启动。\n") 
                           : tr("Failed to start app.\n", "启动应用失败。\n"));
    }
}

void app_stop_command(const char *app_name) {
    if (!app_name || !*app_name) {
        uart_puts(tr("Usage: app stop <appname>\n", "用法：app stop <应用名>\n"));
        return;
    }
    if (app_stop(app_name) == 0) {
        uart_puts(tr("App stopped.\n", "应用已停止。\n"));
    } else {
        uart_puts(tr("Failed to stop app (not running?).\n", "停止应用失败（可能未运行）。\n"));
    }
}

void app_logs_command(const char *app_name) {
    if (!app_name || !*app_name) {
        uart_puts(tr("Usage: app logs <appname>\n", "用法：app logs <应用名>\n"));
        return;
    }
    char *logs = app_get_logs(app_name);
    if (logs) {
        uart_puts(logs);
        free(logs);
    } else {
        uart_puts(tr("No logs found.\n", "未找到日志。\n"));
    }
}

void app_dispatch(const char *cmd_line) {
    if (!cmd_line || !*cmd_line) {
        uart_puts(tr("app: missing subcommand\n", "app: 缺少子命令\n"));
        return;
    }
    char cmd_buf[256];
    strncpy(cmd_buf, cmd_line, sizeof(cmd_buf)-1);
    cmd_buf[sizeof(cmd_buf)-1] = '\0';
    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    if (!subcmd) return;
    if (strcmp(subcmd, "install") == 0) {
        char *pkg = strtok_r(NULL, " ", &saveptr);
        app_install_command(pkg);
    } else if (strcmp(subcmd, "uninstall") == 0) {
        char *name = strtok_r(NULL, " ", &saveptr);
        app_uninstall_command(name);
    } else if (strcmp(subcmd, "list") == 0) {
        app_list_command();
    } else if (strcmp(subcmd, "run") == 0) {
        char *name = strtok_r(NULL, " ", &saveptr);
        app_run_command(name);
    } else if (strcmp(subcmd, "stop") == 0) {
        char *name = strtok_r(NULL, " ", &saveptr);
        app_stop_command(name);
    } else if (strcmp(subcmd, "logs") == 0) {
        char *name = strtok_r(NULL, " ", &saveptr);
        app_logs_command(name);
    } else if (strcmp(subcmd, "daemon") == 0) {
            uart_puts(tr("App daemon not available in command line version.\n",
                         "命令行版本不支持应用守护进程。\n"));
    } else {
        uart_puts(tr("app: unknown subcommand. Available: install, uninstall, list, run, stop, logs, daemon\n",
                     "app: 未知子命令。可用：install, uninstall, list, run, stop, logs, daemon\n"));
    }
}