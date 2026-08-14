/**
 * @file    startup_mode.c
 * @brief   启动模式管理实现
 * @version LN-B-4.2.0.0
 */

#include "startup_mode.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define STARTUP_CONFIG_PATH "/system/config/startup.conf"
#define DEFAULT_MODE STARTUP_MODE_SHELL

/* ============================================================
 * 内部辅助：获取配置文件路径
 * ============================================================ */

static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, STARTUP_CONFIG_PATH);
    }
    return path;
}

/* ============================================================
 * 内部辅助：确保目录存在
 * ============================================================ */

static void ensure_config_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_WARN_T("StartupMode", "EnsureDir", "Fail", "cannot create %s: %s", dir, strerror(errno));
        } else {
            LOG_DEBUG_T("StartupMode", "EnsureDir", "OK", "created %s", dir);
        }
    }
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

startup_mode_t startup_mode_get(void) {
    LOG_DEBUG_T("StartupMode", "Get", "Enter", "Reading startup mode");
    const char *path = get_config_path();

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("StartupMode", "Get", "NoConfig", "config file not found, using default: shell");
        return DEFAULT_MODE;
    }

    char line[64];
    startup_mode_t mode = DEFAULT_MODE;

    /* 跳过注释行与空行，解析 mode = xxx（兼容 mode=tui / tui 格式） */
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* 跳过注释与空行 */
        if (line[0] == '#' || line[0] == '\0') continue;

        char mode_val[32] = {0};
        if (sscanf(line, "mode = %31s", mode_val) == 1 ||
            sscanf(line, "mode=%31s", mode_val) == 1 ||
            sscanf(line, "%31s", mode_val) == 1) {
            if (strcmp(mode_val, "tui") == 0) {
                mode = STARTUP_MODE_TUI;
                LOG_DEBUG_T("StartupMode", "Get", "TUI", "mode = tui");
            } else if (strcmp(mode_val, "shell") == 0) {
                mode = STARTUP_MODE_SHELL;
                LOG_DEBUG_T("StartupMode", "Get", "Shell", "mode = shell");
            } else {
                LOG_WARN_T("StartupMode", "Get", "Unknown", "unknown mode '%s', using default: shell", mode_val);
                mode = DEFAULT_MODE;
            }
            break;
        }
    }
    fclose(fp);

    LOG_DEBUG_T("StartupMode", "Get", "Result", "mode=%d (%s)", mode, startup_mode_name(mode));
    return mode;
}

int startup_mode_set(startup_mode_t mode) {
    LOG_INFO_T("StartupMode", "Set", "Enter", "mode=%d (%s)", mode, startup_mode_name(mode));

    if (mode != STARTUP_MODE_SHELL && mode != STARTUP_MODE_TUI) {
        LOG_ERROR_T("StartupMode", "Set", "Invalid", "unknown mode: %d", mode);
        return -1;
    }

    ensure_config_dir();

    const char *path = get_config_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("StartupMode", "Set", "OpenFail", "cannot write %s: %s (errno=%d)",
                    path, strerror(errno), errno);
        return -1;
    }

    const char *mode_str = startup_mode_name(mode);
    fprintf(fp, "# LING OS Startup Mode\n");
    fprintf(fp, "# shell - Default to Shell CLI\n");
    fprintf(fp, "# tui   - Default to TUI Desktop\n");
    fprintf(fp, "mode = %s\n", mode_str);
    fclose(fp);

    /* 确保立即写入磁盘 */
    sync();

    LOG_INFO_T("StartupMode", "Set", "OK", "startup mode set to %s", mode_str);
    return 0;
}

void startup_mode_show(void) {
    startup_mode_t mode = startup_mode_get();
    uart_puts(tr("Startup mode: ", "启动模式："));
    const char *mode_str = startup_mode_name(mode);
    uart_puts(mode_str);
    uart_puts("\n");
    uart_puts(tr("  shell - Command-line interface (CLI)\n",
                 "  shell - 命令行界面（CLI）\n"));
    uart_puts(tr("  tui   - Desktop interface (TUI)\n",
                 "  tui   - 桌面界面（TUI）\n"));
    uart_puts(tr("\nTo change: system startup shell|tui\n",
                 "\n修改方式：system startup shell|tui\n"));
    LOG_INFO_T("StartupMode", "Show", "OK", "displayed mode: %s", mode_str);
}

const char* startup_mode_name(startup_mode_t mode) {
    switch (mode) {
        case STARTUP_MODE_SHELL:
            return "shell";
        case STARTUP_MODE_TUI:
            return "tui";
        default:
            return "unknown";
    }
}