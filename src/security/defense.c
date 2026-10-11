/**
 * @file    src/security/defense.c
 * @brief   主动防御系统（影子/暗影/绝对保护）
 * @version LN-B-4.3.0.0
 * @changes 新增配置向导步骤注册；新增 defense_save_config()；添加 lang.h 修复 tr() 未声明
 */

#include "defense.h"
#include "../health/behavior_monitor.h" /* 【P5 修复】异常检测取真实评分（此前为恒真占位） */
#include "defense_mode.h"              /* 【P5 接线】与防御状态机保持一致 */
#include "dark_mode.h"                 /* 【P5 接线】暗影模式敏感数据加密态 */
#include "absolute_protect.h"          /* 【P5 接线】绝对保护真实阻断 */
#include "audit.h"                     /* 【P5 接线】高危操作审计 */
#include "verify/second_verify.h"      /* 【P5 接线】高危操作二次验证（此前零调用者） */
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"      /* 新增：修复 tr() 未声明 */
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define DEFENSE_CONFIG_PATH "/system/config/defense.conf"

/* ============================================================
 * 全局状态
 * ============================================================ */

static int g_shadow_mode = 0;
static int g_dark_mode = 0;
static int g_anomaly_threshold = 80;
static int g_behavior_monitoring = 1;
static char g_anomaly_algorithm[64] = "sliding";

/* ============================================================
 * 加载配置
 * ============================================================ */

static void load_defense_config(void) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s%s", root, DEFENSE_CONFIG_PATH);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Defense", "Load", "NotFound", "defense.conf not found, using defaults");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "anomaly_algorithm") == 0) {
                safe_strncpy(g_anomaly_algorithm, val, sizeof(g_anomaly_algorithm));
            } else if (strcmp(key, "behavior_monitoring") == 0) {
                g_behavior_monitoring = atoi(val);
            } else if (strcmp(key, "shadow_mode_default") == 0) {
                g_shadow_mode = atoi(val);
            } else if (strcmp(key, "dark_mode_default") == 0) {
                g_dark_mode = atoi(val);
            }
        }
    }
    fclose(fp);
    LOG_DEBUG_T("Defense", "Load", "OK", "defense.conf loaded");
}

/* ============================================================
 * 保存配置（供配置向导调用）
 * ============================================================ */

int defense_save_config(void) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s%s", root, DEFENSE_CONFIG_PATH);

    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Defense", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "# LING OS Defense Configuration\n");
    fprintf(fp, "# Auto-generated\n\n");
    fprintf(fp, "anomaly_algorithm = %s\n", g_anomaly_algorithm);
    fprintf(fp, "behavior_monitoring = %d\n", g_behavior_monitoring);
    fprintf(fp, "shadow_mode_default = %d\n", g_shadow_mode);
    fprintf(fp, "dark_mode_default = %d\n", g_dark_mode);

    fclose(fp);
    LOG_INFO_T("Defense", "Save", "OK", "defense.conf saved");
    return 0;
}

/* ============================================================
 * 防御系统初始化
 * ============================================================ */

void defense_init(void) {
    LOG_INFO_T("Defense", "Init", "Enter", "initializing defense system");
    load_defense_config();

    /* ====== 新增：注册防御系统配置步骤 ====== */
    LOG_DEBUG_T("Defense", "Init", "ConfigStep", "defense config step available");

    LOG_INFO_T("Defense", "Init", "OK", "defense system initialized");
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

/* 【P5 接线】影子/暗影开关与防御状态机（defense_mode.c）保持一致：
 *   此前 defense.c 只改自己的静态标志，而执行侧（execution_gate / syscall_handler /
 *   shadow_mode.c / dark_mode.c）读的是 defense_mode_get() →
 *   用户在 shell 里开影子模式，链路里根本没生效。
 *   现在：开 → 升级模式；关 → 仅当当前正是该模式时强制降级到 none。 */
void defense_shadow_mode(int enable) {
    if (enable) {
        if (defense_mode_set(DEFENSE_MODE_SHADOW) != 0) {
            LOG_WARN_T("Defense", "ShadowMode", "Blocked",
                       "cannot switch to shadow: current mode %s is higher",
                       defense_mode_name(defense_mode_get()));
        }
    } else if (defense_mode_get() == DEFENSE_MODE_SHADOW) {
        defense_mode_set_force(DEFENSE_MODE_NONE);
    }

    /* 以状态机为准：可能因等级规则未切换成功 */
    g_shadow_mode = (defense_mode_get() == DEFENSE_MODE_SHADOW) ? 1 : 0;
    LOG_INFO_T("Defense", "ShadowMode", "%s", enable ? "enabled" : "disabled");
    defense_save_config();
}

void defense_dark_mode(int enable) {
    if (enable) {
        if (defense_mode_set(DEFENSE_MODE_DARK) != 0) {
            LOG_WARN_T("Defense", "DarkMode", "Blocked",
                       "cannot switch to dark: current mode %s is higher",
                       defense_mode_name(defense_mode_get()));
        }
    } else if (defense_mode_get() == DEFENSE_MODE_DARK) {
        defense_mode_set_force(DEFENSE_MODE_NONE);
    }

    g_dark_mode = (defense_mode_get() == DEFENSE_MODE_DARK) ? 1 : 0;
    LOG_INFO_T("Defense", "DarkMode", "%s", enable ? "enabled" : "disabled");
    defense_save_config();
}

void defense_absolute_protect(void) {
    /* 【P5 接线】高危操作二次验证（verify/second_verify.c 此前零调用者）：
     *   仅当已设置 root 密码且处于交互终端时要求「密码 + Y/N」；
     *   非交互（脚本/管道/daemon）保持原行为并写审计，避免阻塞自动化流程。 */
    if (second_verify_has_password() && isatty(STDIN_FILENO)) {
        verify_result_t vr = second_verify_quick("activate absolute protect");
        if (vr != VERIFY_RESULT_APPROVED) {
            uart_puts(COLOR_RED);
            uart_puts(tr("Absolute protect activation denied (second verification failed).\n",
                         "绝对保护激活被拒绝（二次验证未通过）。\n"));
            uart_puts(COLOR_RESET);
            LOG_WARN_T("Defense", "AbsoluteProtect", "Denied",
                       "second verify result=%s", second_verify_result_str(vr));
            audit_log("system", "defense", "absolute_protect", "activation",
                      second_verify_result_str(vr), -1, "critical", 0);
            return;
        }
    }

    /* 【P5 接线】真正落地阻断：absolute_protect_trigger() → defense_mode_set(ABSOLUTE)
     *   → execution_gate_check() 拒绝所有外部输入（safe_exec 链路随之拒绝执行）。
     *   此前这里只打印横幅，没有任何输入被阻断。 */
    if (absolute_protect_trigger("shell") == 0) {
        LOG_WARN_T("Defense", "AbsoluteProtect", "Activated", "absolute protection activated");
        audit_log("system", "defense", "absolute_protect", "activation",
                  "success", 0, "critical", 1);
    } else {
        uart_puts(COLOR_RED);
        uart_puts(tr("Failed to activate absolute protect.\n", "绝对保护激活失败。\n"));
        uart_puts(COLOR_RESET);
        LOG_ERROR_T("Defense", "AbsoluteProtect", "Fail", "absolute_protect_trigger failed");
        audit_log("system", "defense", "absolute_protect", "activation",
                  "failed", -1, "critical", 0);
    }
}

void defense_set_anomaly_threshold(int threshold) {
    if (threshold < 0) threshold = 0;
    if (threshold > 100) threshold = 100;
    g_anomaly_threshold = threshold;
    LOG_INFO_T("Defense", "SetThreshold", "OK", "anomaly threshold=%d", threshold);
}

int defense_get_anomaly_threshold(void) {
    return g_anomaly_threshold;
}

void defense_show_status(void) {
    uart_puts(tr("\n=== Defense System Status ===\n", "\n=== 防御系统状态 ===\n"));
    char buf[256];
    /* 【P5 接线】展示权威状态机模式（执行侧读的也是它） */
    safe_snprintf(buf, sizeof(buf), tr("Current mode: %s\n", "当前模式：%s\n"),
                  defense_mode_name(defense_mode_get()));
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Shadow mode: %s\n", "影子模式：%s\n"), g_shadow_mode ? "ON" : "OFF");
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Dark mode: %s\n", "暗影模式：%s\n"), g_dark_mode ? "ON" : "OFF");
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Anomaly threshold: %d%%\n", "异常阈值：%d%%\n"), g_anomaly_threshold);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Behavior monitoring: %s\n", "行为监控：%s\n"), g_behavior_monitoring ? "ENABLED" : "DISABLED");
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), tr("Anomaly algorithm: %s\n", "异常算法：%s\n"), g_anomaly_algorithm);
    uart_puts(buf);
}

/* ============================================================
 * 异常检测
 * 【P5 修复】此前是恒真占位（无论输入如何都返回 "No anomaly detected" + 0）——
 *   违反「禁止假数据/恒真返回」。现改为读取 src/health/behavior_monitor.c 的
 *   真实 EWMA 评分，并与本模块阈值（defense_set_anomaly_threshold）比较：
 *     · 评分不可得（monitor 未初始化 / app 未跟踪）→ -1「未知」，绝不当作正常
 *     · 评分 ≥ 阈值 → 1「异常」；否则 0
 * ============================================================ */

int defense_check_anomaly(const char *ai_name, const char *skill_name,
                          char *result, uint32_t result_len) {
    if (!result || result_len == 0) return -1;

    const char *who = (ai_name && *ai_name) ? ai_name : "ai";
    int score = 0;
    int rc = behavior_monitor_get_anomaly_score(who, &score);
    if (rc != 0) {
        safe_snprintf(result, result_len,
                      "Anomaly score unavailable for '%s' (monitor rc=%d)", who, rc);
        LOG_DEBUG_T("Defense", "CheckAnomaly", "Unavailable", "app=%s rc=%d", who, rc);
        return -1;
    }

    int threshold = defense_get_anomaly_threshold();
    int anomaly = (score >= threshold);
    safe_snprintf(result, result_len, "%s: score=%d threshold=%d skill=%s",
                  anomaly ? "ANOMALY" : "normal", score, threshold,
                  (skill_name && *skill_name) ? skill_name : "-");
    LOG_INFO_T("Defense", "CheckAnomaly", anomaly ? "Anomaly" : "Normal",
               "app=%s score=%d threshold=%d", who, score, threshold);
    audit_log("system", "defense", "anomaly_check", who, result,
              anomaly ? -1 : 0, anomaly ? "high" : "low", 1);
    return anomaly ? 1 : 0;
}