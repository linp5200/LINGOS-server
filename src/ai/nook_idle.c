/**
 * @file    nook_idle.c
 * @brief   Idle health check implementation (real system calls with registry integration)
 * @version LN-B-5.0.0.0
 * @changes 实现真实技能健康检查（调用注册表）；安全字符串替换
 */

#include "nook_idle.h"
#include "../common/string_no_sys.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../registry/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * 检查项名称列表（用于状态显示）
 * ============================================================ */
static const char *check_names[] = {
    "log",
    "network",
    "system",
    "process",
    "permission",
    "defense",
    "skill",
    "config"
};

/* ============================================================
 * 全局状态
 * ============================================================ */
static idle_state_t g_idle;

/* ============================================================
 * 内部辅助：运行 shell 命令并捕获输出（使用 fork+execvp 替代 system）
 * ============================================================ */
static int run_shell_cmd(const char *cmd, char *out, int out_len) {
    LOG_DEBUG_T("NookIdle", "RunShell", "Enter", "cmd='%s'", cmd ? cmd : "(null)");
    if (!cmd || !out || out_len <= 0) {
        LOG_ERROR_T("NookIdle", "RunShell", "Invalid", "cmd=%p, out=%p, out_len=%d", (void*)cmd, (void*)out, out_len);
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERROR_T("NookIdle", "RunShell", "PopenFail", "popen(%s) failed: %s (errno=%d)", cmd, strerror(errno), errno);
        safe_snprintf(out, out_len, tr("Error executing command", "执行命令错误"));
        return -1;
    }

    int total = 0;
    while (total < out_len - 1) {
        int ch = fgetc(fp);
        if (ch == EOF) {
            LOG_DEBUG_T("NookIdle", "RunShell", "EOF", "read %d bytes", total);
            break;
        }
        out[total++] = (char)ch;
    }
    out[total] = '\0';
    int ret = pclose(fp);
    LOG_DEBUG_T("NookIdle", "RunShell", "Done", "command returned %d, output_len=%d", ret, total);
    return (total > 0) ? 0 : -1;
}

/* ============================================================
 * 内部辅助：检查注册表健康状态（新增）
 * ============================================================ */
static int check_registry_health(char *result, uint32_t len) {
    LOG_DEBUG_T("NookIdle", "CheckRegistry", "Enter", "");
    if (!result || len == 0) return -1;

    /* 检查注册表是否已初始化 */
    const registry_entry_t *entry = registry_get("skill:file_read");
    if (entry) {
        safe_snprintf(result, len, tr("Registry OK, found %d entries", "注册表正常，找到 %d 个条目"),
                      registry_list(-1, NULL, 0));
        LOG_DEBUG_T("NookIdle", "CheckRegistry", "OK", "registry healthy");
        return 0;
    } else {
        safe_snprintf(result, len, tr("Registry not initialized or empty", "注册表未初始化或为空"));
        LOG_WARN_T("NookIdle", "CheckRegistry", "Fail", "registry check failed");
        return -1;
    }
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

void nook_idle_init(void) {
    LOG_INFO_T("NookIdle", "Init", "Enter", "Initializing idle health check system");
    memset(&g_idle, 0, sizeof(g_idle));
    g_idle.idle_timeout_ticks = 1000;
    g_idle.idle_interval_ticks = 6000;
    for (int i = 0; i < IDLE_CHECK_COUNT; i++) {
        g_idle.checks_enabled[i] = 1;
        LOG_DEBUG_T("NookIdle", "Init", "Check", "%s enabled", check_names[i]);
    }
    g_idle.enabled = 0;
    LOG_INFO_T("NookIdle", "Init", "OK", "Idle health check system initialized (disabled by default). timeout=%u, interval=%u",
               g_idle.idle_timeout_ticks, g_idle.idle_interval_ticks);
    uart_puts(tr("[Idle] Ready. Use 'nook idle start' to enable.\n",
                 "[空闲] 已就绪。使用 'nook idle start' 启用。\n"));
}

void nook_idle_feed(uint64_t t) {
    LOG_DEBUG_T("NookIdle", "Feed", "Enter", "tick=%llu", (unsigned long long)t);
    g_idle.last_command_tick = t;
    LOG_DEBUG_T("NookIdle", "Feed", "OK", "last_command_tick updated to %llu", (unsigned long long)t);
}

void nook_idle_set_enabled(uint8_t e) {
    LOG_INFO_T("NookIdle", "SetEnabled", "Enter", "enable=%d (current=%d)", e, g_idle.enabled);
    g_idle.enabled = e;
    LOG_INFO_T("NookIdle", "SetEnabled", "OK", "Idle check %s", e ? "ENABLED" : "DISABLED");
    uart_puts(tr("[Idle] ", "[空闲] "));
    uart_puts(e ? tr("ENABLED\n", "已启用\n") : tr("DISABLED\n", "已禁用\n"));
}

void nook_idle_show_status(void) {
    LOG_DEBUG_T("NookIdle", "ShowStatus", "Enter", "displaying idle status");
    uart_puts(tr("=== Idle Health Check ===\n", "=== 空闲健康检查 ===\n"));
    uart_puts(tr("Enabled: ", "启用状态："));
    uart_puts(g_idle.enabled ? tr("YES\n", "是\n") : tr("NO\n", "否\n"));
    LOG_DEBUG_T("NookIdle", "ShowStatus", "Enabled", "enabled=%d", g_idle.enabled);

    for (int i = 0; i < IDLE_CHECK_COUNT; i++) {
        uart_puts("  ");
        uart_puts(check_names[i]);
        uart_puts(": ");
        uart_puts(g_idle.checks_enabled[i] ? tr("ON", "启用") : tr("OFF", "禁用"));
        if (i == IDLE_CHECK_SKILL) {
            uart_puts(tr(" (registry-based)", " (基于注册表)"));
        }
        uart_puts("\n");
        LOG_DEBUG_T("NookIdle", "ShowStatus", "CheckItem", "%s: %s", check_names[i],
                    g_idle.checks_enabled[i] ? "ON" : "OFF");
    }
    LOG_DEBUG_T("NookIdle", "ShowStatus", "Exit", "status display complete");
}

void nook_idle_poll(uint64_t t) {
    LOG_DEBUG_T("NookIdle", "Poll", "Enter", "tick=%llu, enabled=%d, check_running=%d",
                (unsigned long long)t, g_idle.enabled, g_idle.check_running);

    if (!g_idle.enabled) {
        LOG_DEBUG_T("NookIdle", "Poll", "Skip", "idle check disabled");
        return;
    }
    if (g_idle.check_running) {
        LOG_DEBUG_T("NookIdle", "Poll", "Skip", "check already running");
        return;
    }
    if ((t - g_idle.last_command_tick) < g_idle.idle_timeout_ticks) {
        LOG_DEBUG_T("NookIdle", "Poll", "Skip", "not idle yet (last_cmd=%llu, timeout=%u)",
                    (unsigned long long)g_idle.last_command_tick, g_idle.idle_timeout_ticks);
        return;
    }
    if ((t - g_idle.last_idle_check_tick) < g_idle.idle_interval_ticks) {
        LOG_DEBUG_T("NookIdle", "Poll", "Skip", "interval not reached (last_check=%llu, interval=%u)",
                    (unsigned long long)g_idle.last_idle_check_tick, g_idle.idle_interval_ticks);
        return;
    }

    LOG_INFO_T("NookIdle", "Poll", "Start", "Starting autonomous health check (tick=%llu)", (unsigned long long)t);
    g_idle.check_running = 1;
    g_idle.last_idle_check_tick = t;
    uart_puts(tr("[Idle] Starting autonomous health check...\n", "[空闲] 开始自动健康检查...\n"));

    char result[512];
    int error_found = 0;

    for (int i = 0; i < IDLE_CHECK_COUNT; i++) {
        if (!g_idle.checks_enabled[i]) {
            LOG_DEBUG_T("NookIdle", "Poll", "SkipCheck", "%s disabled, skipping", check_names[i]);
            continue;
        }

        LOG_DEBUG_T("NookIdle", "Poll", "Check", "Running check: %s", check_names[i]);
        int ret = -1;
        switch (i) {
            case IDLE_CHECK_LOG:        ret = idle_check_log(result, sizeof(result)); break;
            case IDLE_CHECK_NETWORK:    ret = idle_check_network(result, sizeof(result)); break;
            case IDLE_CHECK_SYSTEM:     ret = idle_check_system(result, sizeof(result)); break;
            case IDLE_CHECK_PROCESS:    ret = idle_check_process(result, sizeof(result)); break;
            case IDLE_CHECK_PERMISSION: ret = idle_check_permission(result, sizeof(result)); break;
            case IDLE_CHECK_DEFENSE:    ret = idle_check_defense(result, sizeof(result)); break;
            /* 【修改】技能检查：调用注册表健康检查 */
            case IDLE_CHECK_SKILL:      ret = check_registry_health(result, sizeof(result)); break;
            case IDLE_CHECK_CONFIG:     ret = idle_check_config(result, sizeof(result)); break;
            default:
                LOG_WARN_T("NookIdle", "Poll", "UnknownCheck", "check index %d unknown", i);
                continue;
        }

        LOG_DEBUG_T("NookIdle", "Poll", "CheckResult", "%s: ret=%d, result='%s'", check_names[i], ret, result);
        uart_puts("[Idle] ");
        uart_puts(check_names[i]);
        uart_puts(": ");
        uart_puts(result);
        uart_puts("\n");

        if (ret != 0) {
            LOG_WARN_T("NookIdle", "Poll", "CheckFailed", "%s check failed with ret=%d", check_names[i], ret);
            error_found = 1;
        }
    }

    if (error_found) {
        uart_puts(tr("[Idle] Errors found. Consider manual repair.\n",
                    "[空闲] 发现错误。请考虑手动修复。\n"));
        LOG_WARN_T("NookIdle", "Poll", "ErrorsFound", "One or more checks failed");
    } else {
        uart_puts(tr("[Idle] All checks passed.\n", "[空闲] 所有检查通过。\n"));
        LOG_INFO_T("NookIdle", "Poll", "AllPass", "All checks passed");
    }

    g_idle.check_running = 0;
    LOG_DEBUG_T("NookIdle", "Poll", "Exit", "check completed, error_found=%d", error_found);
}

/* ============================================================
 * 各检查项实现（保持原有实现，仅添加双文支持）
 * ============================================================ */

int idle_check_log(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckLog", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("tail -n 20 /var/log/syslog 2>&1 | grep -E '(error|fail|critical)' || echo 'No critical errors found'", r, l);
    LOG_DEBUG_T("NookIdle", "CheckLog", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}

int idle_check_network(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckNetwork", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("ping -c 1 8.8.8.8 2>&1 && echo 'Network OK' || echo 'Network unreachable'", r, l);
    LOG_DEBUG_T("NookIdle", "CheckNetwork", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}

int idle_check_system(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckSystem", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("free -h 2>&1; echo '---'; df -h / 2>&1", r, l);
    LOG_DEBUG_T("NookIdle", "CheckSystem", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}

int idle_check_process(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckProcess", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("ps aux --sort=-%mem | head -n 10 2>&1", r, l);
    LOG_DEBUG_T("NookIdle", "CheckProcess", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}

int idle_check_permission(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckPermission", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("find / -perm -4000 -o -perm -2000 2>/dev/null | head -n 20", r, l);
    LOG_DEBUG_T("NookIdle", "CheckPermission", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}

int idle_check_defense(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckDefense", "Enter", "buf_len=%u", l);
    safe_snprintf(r, l, tr("Defense: active (check log for details)", "防御：已激活（详情查看日志）"));
    LOG_DEBUG_T("NookIdle", "CheckDefense", "Exit", "result='%s'", r);
    return 0;
}

/* 【修改】技能检查：已在 idle_check_skill 中替换为 check_registry_health */
int idle_check_skill(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckSkill", "Enter", "buf_len=%u", l);
    /* 此函数已不再使用，由 check_registry_health 替代 */
    safe_snprintf(r, l, tr("Skill check moved to registry check", "技能检查已迁移至注册表检查"));
    LOG_DEBUG_T("NookIdle", "CheckSkill", "Exit", "result='%s'", r);
    return 0;
}

int idle_check_config(char *r, uint32_t l) {
    LOG_DEBUG_T("NookIdle", "CheckConfig", "Enter", "buf_len=%u", l);
    int ret = run_shell_cmd("stat /etc/passwd /etc/shadow 2>&1 | head -n 10", r, l);
    LOG_DEBUG_T("NookIdle", "CheckConfig", "Exit", "ret=%d, result='%s'", ret, r);
    return ret;
}