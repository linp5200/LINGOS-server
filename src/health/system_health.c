/**
 * @file    system_health.c
 * @brief   系统健康检查命令实现（含趋势记录和后台告警）
 * @version 2.0.0.0
 */

#include "system_health.h"
#include "health_trend.h"
#include "../common/lang.h"
#include "uart.h"
#include "../core/version.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "../security/audit.h"
#include "../common/interactive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

int get_memory_usage(void) {
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        unsigned long total = info.totalram;
        unsigned long free = info.freeram;
        if (total > 0) return (int)((total - free) * 100 / total);
    }
    return -1;
}

int get_disk_usage(const char *path) {
    struct statvfs stat;
    if (statvfs(path, &stat) == 0) {
        unsigned long total = stat.f_blocks * stat.f_frsize;
        unsigned long free = stat.f_bfree * stat.f_frsize;
        if (total > 0) return (int)((total - free) * 100 / total);
    }
    return -1;
}

void get_load_avg(double *load1, double *load5, double *load15) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) {
        fscanf(fp, "%lf %lf %lf", load1, load5, load15);
        fclose(fp);
    } else {
        *load1 = *load5 = *load15 = -1.0;
    }
}

int check_python(void) {
    if (system("python3 --version > /dev/null 2>&1") != 0) return 0;
    if (system("python3 -c 'import flask' > /dev/null 2>&1") != 0) return 0;
    if (system("python3 -c 'import requests' > /dev/null 2>&1") != 0) return 0;
    return 1;
}

int check_ai_backend(void) {
    FILE *fp = popen("curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080 2>/dev/null", "r");
    if (!fp) return 0;
    char code[4] = {0};
    if (fgets(code, sizeof(code), fp)) {
        int http_code = atoi(code);
        pclose(fp);
        return (http_code == 200 || http_code == 404) ? 1 : 0;
    }
    pclose(fp);
    return 0;
}

int check_network(void) {
    FILE *fp = popen("curl -s -o /dev/null -w '%{http_code}' --connect-timeout 5 https://api.deepseek.com/health 2>/dev/null", "r");
    if (!fp) return 0;
    char code[4] = {0};
    if (fgets(code, sizeof(code), fp)) {
        int http_code = atoi(code);
        pclose(fp);
        return (http_code == 200) ? 1 : 0;
    }
    pclose(fp);
    return 0;
}

void health_check_and_alert(void) {
    int mem_usage = get_memory_usage();
    const char *root = lingos_data_root();
    int disk_usage = get_disk_usage(root);
    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);
    int python_ok = check_python();
    int ai_ok = check_ai_backend();
    int net_ok = check_network();

    health_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.timestamp = time(NULL);
    rec.mem_usage = mem_usage;
    rec.disk_usage = disk_usage;
    rec.load_avg = load1;
    rec.python_ok = python_ok;
    rec.ai_backend_ok = ai_ok;
    rec.net_ok = net_ok;
    health_trend_record(&rec);

    int alert = 0;
    char alert_msg[512] = {0};
    if (mem_usage > 90) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "High memory usage: %d%%; ", mem_usage);
        alert = 1;
    }
    if (disk_usage > 85) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "High disk usage: %d%%; ", disk_usage);
        alert = 1;
    }
    if (load1 > 2.0) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "High load average: %.2f; ", load1);
        alert = 1;
    }
    if (!python_ok) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "Python missing; ");
        alert = 1;
    }
    if (!ai_ok) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "AI backend unreachable; ");
        alert = 1;
    }
    if (!net_ok) {
        snprintf(alert_msg + strlen(alert_msg), sizeof(alert_msg) - strlen(alert_msg),
                 "Network unreachable; ");
        alert = 1;
    }
    if (alert) {
        audit_log("system", "health_watchdog", "health_alert", alert_msg, "", 0, "medium", 1);
        if (lingos_is_interactive) {
            uart_puts(tr("\n[HEALTH ALERT] ", "\n[健康告警] "));
            uart_puts(alert_msg);
            uart_puts("\n");
        }
        LOG_WARN_T("HealthWatchdog", "Alert", "Trigger", "%s", alert_msg);
    }
}

void system_health_command(void) {
    uart_puts(tr("\n=== System Health ===\n", "\n=== 系统健康状态 ===\n"));

    char buf[256];
    snprintf(buf, sizeof(buf), tr("Version: %s\n", "版本: %s\n"), version_get());
    uart_puts(buf);

    int mem_usage = get_memory_usage();
    if (mem_usage >= 0) {
        snprintf(buf, sizeof(buf), tr("Memory usage: %d%%\n", "内存使用率: %d%%\n"), mem_usage);
        uart_puts(buf);
        if (mem_usage > 90) uart_puts(tr("  [WARN] High memory usage\n", "  [警告] 内存使用过高\n"));
    } else {
        uart_puts(tr("Memory usage: N/A\n", "内存使用率: 未知\n"));
    }

    const char *root = lingos_data_root();
    int disk_usage = get_disk_usage(root);
    if (disk_usage >= 0) {
        snprintf(buf, sizeof(buf), tr("Disk (%s) usage: %d%%\n", "磁盘 (%s) 使用率: %d%%\n"), root, disk_usage);
        uart_puts(buf);
        if (disk_usage > 85) uart_puts(tr("  [WARN] High disk usage\n", "  [警告] 磁盘使用过高\n"));
    } else {
        snprintf(buf, sizeof(buf), tr("Disk (%s) usage: N/A\n", "磁盘 (%s) 使用率: 未知\n"), root);
        uart_puts(buf);
    }

    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);
    if (load1 >= 0) {
        snprintf(buf, sizeof(buf), tr("CPU load: %.2f, %.2f, %.2f\n", "CPU 负载: %.2f, %.2f, %.2f\n"), load1, load5, load15);
        uart_puts(buf);
        if (load1 > 2.0) uart_puts(tr("  [WARN] High CPU load\n", "  [警告] CPU 负载过高\n"));
    } else {
        uart_puts(tr("CPU load: N/A\n", "CPU 负载: 未知\n"));
    }

    int python_ok = check_python();
    uart_puts(python_ok ? tr("Python: OK (flask, requests installed)\n", "Python: 正常 (flask, requests 已安装)\n")
                        : tr("Python: MISSING or missing dependencies (flask, requests)\n", "Python: 缺失或缺少依赖 (flask, requests)\n"));

    int ai_ok = check_ai_backend();
    uart_puts(ai_ok ? tr("AI backend: reachable\n", "AI 后端: 可访问\n")
                    : tr("AI backend: NOT reachable (is ollama_server.py running?)\n", "AI 后端: 不可访问 (ollama_server.py 是否运行？)\n"));

    int net_ok = check_network();
    uart_puts(net_ok ? tr("External network: reachable (DeepSeek API)\n", "外部网络: 可访问 (DeepSeek API)\n")
                     : tr("External network: NOT reachable (check internet connection)\n", "外部网络: 不可访问 (请检查网络连接)\n"));

    uart_puts(tr("=== End ===\n", "=== 结束 ===\n"));

    health_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.timestamp = time(NULL);
    rec.mem_usage = mem_usage;
    rec.disk_usage = disk_usage;
    rec.load_avg = load1;
    rec.python_ok = python_ok ? 1 : 0;
    rec.ai_backend_ok = ai_ok ? 1 : 0;
    rec.net_ok = net_ok ? 1 : 0;
    health_trend_record(&rec);
}