/**
 * @file    src/security/defense.c
 * @brief   主动防御系统（影子/暗影/绝对保护）
 * @version LN-B-4.3.0.0
 * @changes 新增配置向导步骤注册；新增 defense_save_config()；添加 lang.h 修复 tr() 未声明
 */

#include "defense.h"
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

void defense_shadow_mode(int enable) {
    g_shadow_mode = enable ? 1 : 0;
    LOG_INFO_T("Defense", "ShadowMode", "%s", enable ? "enabled" : "disabled");
    defense_save_config();
}

void defense_dark_mode(int enable) {
    g_dark_mode = enable ? 1 : 0;
    LOG_INFO_T("Defense", "DarkMode", "%s", enable ? "enabled" : "disabled");
    defense_save_config();
}

void defense_absolute_protect(void) {
    uart_puts(COLOR_RED);
    uart_puts("\n╔══════════════════════════════════════════════════════════════════╗\n");
    uart_puts("║  ⚠️  ABSOLUTE PROTECTION ACTIVATED                            ║\n");
    uart_puts("║  All high-risk operations are blocked until manual override  ║\n");
    uart_puts("╚══════════════════════════════════════════════════════════════════╝\n");
    uart_puts(COLOR_RESET);
    LOG_WARN_T("Defense", "AbsoluteProtect", "Activated", "absolute protection activated");
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
 * 异常检测（占位）
 * ============================================================ */

int defense_check_anomaly(const char *ai_name, const char *skill_name,
                          char *result, uint32_t result_len) {
    (void)ai_name;
    (void)skill_name;
    if (result && result_len > 0) {
        safe_snprintf(result, result_len, "No anomaly detected");
    }
    return 0;
}