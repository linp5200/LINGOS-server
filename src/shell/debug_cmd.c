/**
 * @file    debug_cmd.c
 * @brief   debug 命令实现：动态调整日志级别
 * @version 2.2.0.0
 */

#include "debug_cmd.h"
#include "uart.h"
#include "log_extra.h"
#include "lang.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *level_names[] = {"ERROR", "WARN", "INFO", "DEBUG"};

static int parse_level(const char *s) {
    if (!s) return -1;
    if (strcasecmp(s, "ERROR") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(s, "WARN") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(s, "INFO") == 0) return LOG_LEVEL_INFO;
    if (strcasecmp(s, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    return -1;
}

void debug_command(const char *args) {
    if (!args || !*args) {
        uart_puts(tr("Usage: debug set module=<module> level=<level>\n"
                     "       debug list\n"
                     "       debug show\n",
                     "用法：debug set module=<模块> level=<级别>\n"
                     "      debug list\n"
                     "      debug show\n"));
        uart_puts(tr("Levels: ERROR, WARN, INFO, DEBUG\n", "级别：ERROR, WARN, INFO, DEBUG\n"));
        return;
    }

    // 解析子命令
    const char *cmd = args;
    while (*cmd == ' ') cmd++;

    if (strncmp(cmd, "set", 3) == 0) {
        cmd += 3;
        while (*cmd == ' ') cmd++;
        // 解析 module=xxx level=yyy
        char module[64] = {0};
        int level = -1;
        char *p = strstr(cmd, "module=");
        if (p) {
            p += 7;
            char *q = strchr(p, ' ');
            if (q) {
                int len = q - p;
                if (len > 63) len = 63;
                strncpy(module, p, len);
                module[len] = '\0';
                p = q;
            } else {
                strncpy(module, p, sizeof(module)-1);
                p += strlen(module);
            }
        }
        p = strstr(cmd, "level=");
        if (p) {
            p += 6;
            char level_str[16] = {0};
            char *q = strchr(p, ' ');
            if (q) {
                int len = q - p;
                if (len > 15) len = 15;
                strncpy(level_str, p, len);
                level_str[len] = '\0';
            } else {
                strncpy(level_str, p, sizeof(level_str)-1);
            }
            level = parse_level(level_str);
        }
        if (level < 0 || module[0] == '\0') {
            uart_puts(tr("Invalid parameters. Usage: debug set module=<module> level=<level>\n",
                         "无效参数。用法：debug set module=<模块> level=<级别>\n"));
            return;
        }
        log_set_module_level(module, level);
        char buf[128];
        snprintf(buf, sizeof(buf), tr("Module '%s' log level set to %s\n",
                                      "模块 '%s' 日志级别设置为 %s\n"),
                 module, level_names[level-1]);
        uart_puts(buf);
        return;
    }

    if (strncmp(cmd, "list", 4) == 0) {
        uart_puts(tr("Available modules:\n", "可用模块：\n"));
        const char *modules[] = {
            "Main", "Nook", "Defense", "SkillExec", "Lingosd",
            "Audit", "Permission", "SelfCheck", "HealthWatchdog",
            "AIConfig", "AppSandbox", "EnvBootstrap", "Update", NULL
        };
        for (int i = 0; modules[i]; i++) {
            uart_puts("  ");
            uart_puts(modules[i]);
            uart_puts("\n");
        }
        return;
    }

    if (strncmp(cmd, "show", 4) == 0) {
        uart_puts(tr("Current module log levels:\n", "当前模块日志级别：\n"));
        // 这里简单打印全部模块的级别（实际实现应读取内部状态）
        log_dump_module_levels();
        return;
    }

    uart_puts(tr("Unknown debug subcommand.\n", "未知的 debug 子命令。\n"));
}