/**
 * @file    privilege_manager.c
 * @brief   开发者模式管理（方案一/方案二）
 * @version LN-B-5.0.0.0
 * @fix     增加 24 小时倒计时自动恢复和重启提醒
 */
#include "privilege_manager.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <pthread.h>

#define PRIVILEGE_CONFIG "/system/config/privilege.json"
#define PRIVILEGE_LOCK "/Ensystem/privilege_lock"
#define MAX_REBOOT_ATTEMPTS 3
#define EXPIRE_SECONDS 86400  /* 24小时 */

typedef struct {
    char mode[16];          /* default | developer */
    time_t activated_at;
    int reminder_interval;
    int auto_revert;
    int reboot_count;
} privilege_config_t;

static privilege_config_t g_priv_cfg;
static int g_loaded = 0;
static pthread_mutex_t g_priv_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助
 * ============================================================ */

static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, PRIVILEGE_CONFIG);
    }
    return path;
}

static const char* get_lock_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, PRIVILEGE_LOCK);
    }
    return path;
}

static int ensure_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) return -1;
    }
    safe_snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) return -1;
    }
    return 0;
}

static void set_defaults(void) {
    memset(&g_priv_cfg, 0, sizeof(privilege_config_t));
    safe_strncpy(g_priv_cfg.mode, "default", sizeof(g_priv_cfg.mode));
    g_priv_cfg.activated_at = 0;
    g_priv_cfg.reminder_interval = 300;
    g_priv_cfg.auto_revert = 1;
    g_priv_cfg.reboot_count = 0;
}

int privilege_config_load(void) {
    pthread_mutex_lock(&g_priv_lock);

    set_defaults();

    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        pthread_mutex_unlock(&g_priv_lock);
        LOG_DEBUG_T("PrivilegeManager", "Load", "NotFound", "using defaults");
        g_loaded = 1;
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); pthread_mutex_unlock(&g_priv_lock); return -1; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (root) {
        cJSON *item;
        item = cJSON_GetObjectItem(root, "mode");
        if (item && cJSON_IsString(item)) {
            safe_strncpy(g_priv_cfg.mode, item->valuestring, sizeof(g_priv_cfg.mode));
        }
        item = cJSON_GetObjectItem(root, "activated_at");
        if (item && cJSON_IsNumber(item)) {
            g_priv_cfg.activated_at = (time_t)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "reminder_interval");
        if (item && cJSON_IsNumber(item)) {
            g_priv_cfg.reminder_interval = item->valueint;
        }
        item = cJSON_GetObjectItem(root, "auto_revert");
        if (item && cJSON_IsBool(item)) {
            g_priv_cfg.auto_revert = cJSON_IsTrue(item);
        }
        item = cJSON_GetObjectItem(root, "reboot_count");
        if (item && cJSON_IsNumber(item)) {
            g_priv_cfg.reboot_count = item->valueint;
        }
        cJSON_Delete(root);
    }

    g_loaded = 1;
    pthread_mutex_unlock(&g_priv_lock);

    LOG_DEBUG_T("PrivilegeManager", "Load", "OK", "mode=%s, activated_at=%ld",
                g_priv_cfg.mode, (long)g_priv_cfg.activated_at);
    return 0;
}

int privilege_config_save(void) {
    pthread_mutex_lock(&g_priv_lock);

    ensure_dir();

    const char *path = get_config_path();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", g_priv_cfg.mode);
    cJSON_AddNumberToObject(root, "activated_at", (double)g_priv_cfg.activated_at);
    cJSON_AddNumberToObject(root, "reminder_interval", g_priv_cfg.reminder_interval);
    cJSON_AddBoolToObject(root, "auto_revert", g_priv_cfg.auto_revert);
    cJSON_AddNumberToObject(root, "reboot_count", g_priv_cfg.reboot_count);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        pthread_mutex_unlock(&g_priv_lock);
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        pthread_mutex_unlock(&g_priv_lock);
        return -1;
    }
    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    pthread_mutex_unlock(&g_priv_lock);
    LOG_DEBUG_T("PrivilegeManager", "Save", "OK", "saved");
    return 0;
}

/* ============================================================
 * 开发者模式管理
 * ============================================================ */

int privilege_set_developer(int enable) {
    LOG_INFO_T("PrivilegeManager", "SetDeveloper", "Enter", "enable=%d", enable);

    const char *lock_path = get_lock_path();
    if (access(lock_path, F_OK) == 0) {
        LOG_WARN_T("PrivilegeManager", "SetDeveloper", "Locked", "system is locked");
        uart_puts(tr(
            "System is locked. Cannot change privilege mode.\n",
            "系统已锁定。无法更改权限模式。\n"
        ));
        return -1;
    }

    privilege_config_load();

    pthread_mutex_lock(&g_priv_lock);

    if (enable) {
        safe_strncpy(g_priv_cfg.mode, "developer", sizeof(g_priv_cfg.mode));
        g_priv_cfg.activated_at = time(NULL);
        g_priv_cfg.reboot_count = 0;
        LOG_INFO_T("PrivilegeManager", "SetDeveloper", "Enabled", "developer mode enabled for 24 hours");
    } else {
        safe_strncpy(g_priv_cfg.mode, "default", sizeof(g_priv_cfg.mode));
        g_priv_cfg.activated_at = 0;
        LOG_INFO_T("PrivilegeManager", "SetDeveloper", "Disabled", "developer mode disabled");
    }

    pthread_mutex_unlock(&g_priv_lock);

    privilege_config_save();

    if (enable) {
        uart_puts(COLOR_RED);
        uart_puts(tr(
            "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "  ⚠️  DEVELOPER MODE ENABLED\n"
            "  All processes will run with root privileges\n"
            "  Please be careful with commands to avoid dangerous operations\n"
            "  Auto-revert in: 24 hours\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n",
            "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "  ⚠️  开发者模式已启用\n"
            "  所有进程将以 root 权限运行\n"
            "  请注意命令输入，避免执行危险操作\n"
            "  自动恢复时间: 24 小时\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        ));
        uart_puts(COLOR_RESET);
    } else {
        uart_puts(tr(
            "Developer mode disabled. System is now in default mode.\n",
            "开发者模式已禁用。系统现在处于默认模式。\n"
        ));
    }

    return 0;
}

int privilege_get_mode(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    privilege_config_load();
    safe_strncpy(out, g_priv_cfg.mode, out_len);
    return 0;
}

long privilege_get_remaining_seconds(void) {
    privilege_config_load();
    if (strcmp(g_priv_cfg.mode, "developer") != 0) return 0;
    time_t now = time(NULL);
    long elapsed = now - g_priv_cfg.activated_at;
    long remaining = EXPIRE_SECONDS - elapsed;
    return remaining < 0 ? 0 : remaining;
}

int privilege_is_locked(void) {
    const char *lock_path = get_lock_path();
    return (access(lock_path, F_OK) == 0);
}

/* ============================================================
 * 自动恢复（含倒计时检查和重启尝试）
 * ============================================================ */

int privilege_revert_to_default(void) {
    LOG_INFO_T("PrivilegeManager", "Revert", "Enter", "reverting to default mode");

    if (privilege_is_locked()) {
        LOG_WARN_T("PrivilegeManager", "Revert", "Locked", "system is locked");
        return -1;
    }

    privilege_config_load();

    pthread_mutex_lock(&g_priv_lock);
    safe_strncpy(g_priv_cfg.mode, "default", sizeof(g_priv_cfg.mode));
    g_priv_cfg.activated_at = 0;
    g_priv_cfg.reboot_count = 0;
    pthread_mutex_unlock(&g_priv_lock);

    privilege_config_save();

    uart_puts(tr(
        "Developer mode has expired. Reverted to default mode.\n",
        "开发者模式已过期，已恢复到默认模式。\n"
    ));

    LOG_INFO_T("PrivilegeManager", "Revert", "OK", "reverted to default mode");

    /* 尝试重启系统（仅当 auto_revert 启用） */
    if (g_priv_cfg.auto_revert) {
        uart_puts(tr(
            "Attempting to restart system to apply changes...\n",
            "正在尝试重启系统以应用更改...\n"
        ));
        sync();
        reboot(RB_AUTOBOOT);
    }

    return 0;
}

/**
 * @brief 检查开发者模式是否过期，若过期则自动恢复
 * @return 1 已恢复，0 未过期或已处理
 */
int privilege_check_and_auto_revert(void) {
    if (privilege_is_locked()) return 0;

    privilege_config_load();

    if (strcmp(g_priv_cfg.mode, "developer") != 0) {
        return 0;
    }

    long remaining = privilege_get_remaining_seconds();
    if (remaining > 0) {
        /* 每 5 分钟提醒一次 */
        static time_t last_reminder = 0;
        time_t now = time(NULL);
        if (now - last_reminder > 300) {
            last_reminder = now;
            long hours = remaining / 3600;
            long mins = (remaining % 3600) / 60;
            uart_puts(COLOR_YELLOW);
            uart_puts(tr(
                "[REMINDER] Developer mode expires in ",
                "[提醒] 开发者模式将在 "
            ));
            char buf[32];
            safe_snprintf(buf, sizeof(buf), "%ld", hours);
            uart_puts(buf);
            uart_puts(tr("h", "小时"));
            safe_snprintf(buf, sizeof(buf), "%ld", mins);
            uart_puts(buf);
            uart_puts(tr("m\n", "分钟\n"));
            uart_puts(COLOR_RESET);
        }
        return 0;
    }

    /* 已过期，自动恢复 */
    LOG_WARN_T("PrivilegeManager", "AutoRevert", "Expired", "developer mode expired, reverting");
    return privilege_revert_to_default();
}

int privilege_increment_reboot_count(void) {
    privilege_config_load();

    pthread_mutex_lock(&g_priv_lock);
    g_priv_cfg.reboot_count++;
    int count = g_priv_cfg.reboot_count;
    pthread_mutex_unlock(&g_priv_lock);

    privilege_config_save();

    if (count >= MAX_REBOOT_ATTEMPTS) {
        const char *lock_path = get_lock_path();
        FILE *fp = fopen(lock_path, "w");
        if (fp) {
            fprintf(fp, "locked_at=%ld\nreason=reboot_loop\nreboot_attempts=%d\n",
                    (long)time(NULL), count);
            fclose(fp);
        }
        LOG_ERROR_T("PrivilegeManager", "IncrementReboot", "Locked",
                    "system locked after %d reboot attempts", count);
        return -1;
    }

    return count;
}