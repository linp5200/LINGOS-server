/**
 * @file    system_info.c
 * @brief   system debug info 命令：收集系统状态信息
 * @version 2.2.0.0
 */

#include "system_info.h"
#include "uart.h"
#include "log_extra.h"
#include "version.h"
#include "data_path.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

void system_debug_info_command(void) {
    uart_puts(tr("=== System Debug Info ===\n", "=== 系统调试信息 ===\n"));

    // 版本信息
    uart_puts(tr("Version: ", "版本："));
    uart_puts(version_get());
    uart_puts("\n");

    // 运行时间
    FILE *fp = fopen("/proc/uptime", "r");
    if (fp) {
        double uptime;
        if (fscanf(fp, "%lf", &uptime) == 1) {
            int days = (int)(uptime / 86400);
            int hours = (int)((uptime - days*86400) / 3600);
            int mins = (int)((uptime - days*86400 - hours*3600) / 60);
            char buf[128];
            snprintf(buf, sizeof(buf), tr("Uptime: %d days, %d hours, %d minutes\n",
                                          "运行时间：%d天 %d小时 %d分钟\n"),
                     days, hours, mins);
            uart_puts(buf);
        }
        fclose(fp);
    } else {
        uart_puts(tr("Uptime: N/A\n", "运行时间：N/A\n"));
    }

    // 系统信息
    uart_puts(tr("System: ", "系统："));
    fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *p = line + 12;
                if (*p == '"') p++;
                char *q = p;
                while (*q && *q != '"') q++;
                *q = '\0';
                uart_puts(p);
                uart_puts("\n");
                break;
            }
        }
        fclose(fp);
    } else {
        uart_puts("Unknown\n");
    }

    // 内核版本
    fp = fopen("/proc/version", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            uart_puts(tr("Kernel: ", "内核："));
            uart_puts(line);
        }
        fclose(fp);
    }

    // 内存信息
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        char total[64] = {0}, free_mem[64] = {0};
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                sscanf(line, "%*s %s", total);
            } else if (strncmp(line, "MemFree:", 8) == 0) {
                sscanf(line, "%*s %s", free_mem);
            }
        }
        fclose(fp);
        if (total[0]) {
            char buf[128];
            snprintf(buf, sizeof(buf), tr("Memory: Total %s kB, Free %s kB\n",
                                          "内存：总计 %s kB，空闲 %s kB\n"),
                     total, free_mem);
            uart_puts(buf);
        }
    }

    // 进程信息
    uart_puts(tr("Key processes:\n", "关键进程：\n"));
    const char *procs[] = {"lingos_linux", "lingosd", "ai_server.py", "sub_ai_worker.py", NULL};
    for (int i = 0; procs[i]; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "pgrep -f '%s' | wc -l", procs[i]);
        FILE *p = popen(cmd, "r");
        if (p) {
            char line[16];
            if (fgets(line, sizeof(line), p)) {
                int count = atoi(line);
                char buf[128];
                snprintf(buf, sizeof(buf), "  %s: %d\n", procs[i], count);
                uart_puts(buf);
            }
            pclose(p);
        }
    }

    // 配置文件状态
    const char *root = lingos_data_root();
    char path[512];
    snprintf(path, sizeof(path), "%s/system/config/ai_config.json", root);
    uart_puts(tr("AI config: ", "AI配置："));
    uart_puts(access(path, F_OK) == 0 ? tr("exists\n", "存在\n") : tr("missing\n", "缺失\n"));

    snprintf(path, sizeof(path), "%s/system/config/roles.json", root);
    uart_puts(tr("Roles config: ", "角色配置："));
    uart_puts(access(path, F_OK) == 0 ? tr("exists\n", "存在\n") : tr("missing\n", "缺失\n"));

    uart_puts(tr("=== End of debug info ===\n", "=== 调试信息结束 ===\n"));
}