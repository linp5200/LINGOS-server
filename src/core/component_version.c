/**
 * @file    component_version.c
 * @brief   组件版本管理（注册、检查兼容性、升级）
 * @version LN-B-4.2.0.0
 */

#include "component_version.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "uart.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_COMPONENTS 32

/* ============================================================
 * 全局状态
 * ============================================================ */

static component_t *g_components[MAX_COMPONENTS];
static int g_component_count = 0;
static char g_version_buf[MAX_COMPONENTS][64];
static int g_initialized = 0;

/* ============================================================
 * 内部辅助：读取版本文件
 * ============================================================ */

static const char* read_version_file(const char *path) {
    LOG_DEBUG_T("Component", "ReadVersion", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        LOG_ERROR_T("Component", "ReadVersion", "Invalid", "path is NULL");
        return NULL;
    }

    static char version[64];
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Component", "ReadVersion", "NotFound", "version file %s not found", path);
        return NULL;
    }

    if (fgets(version, sizeof(version), fp)) {
        char *newline = strchr(version, '\n');
        if (newline) *newline = '\0';
        fclose(fp);
        LOG_DEBUG_T("Component", "ReadVersion", "OK", "version='%s'", version);
        return version;
    }

    fclose(fp);
    LOG_WARN_T("Component", "ReadVersion", "Empty", "version file %s is empty", path);
    return NULL;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int component_register(component_t *comp) {
    LOG_INFO_T("Component", "Register", "Enter", "name='%s'", comp ? comp->name : "(null)");

    if (!comp) {
        LOG_ERROR_T("Component", "Register", "Invalid", "comp is NULL");
        return -1;
    }

    if (g_component_count >= MAX_COMPONENTS) {
        LOG_ERROR_T("Component", "Register", "Overflow", "max components reached (%d)", MAX_COMPONENTS);
        return -1;
    }

    g_components[g_component_count++] = comp;
    LOG_DEBUG_T("Component", "Register", "OK", "registered %s (total=%d)", comp->name, g_component_count);
    return 0;
}

int component_version_init(void) {
    LOG_INFO_T("Component", "Init", "Enter", "initializing component version system");

    if (g_initialized) {
        LOG_WARN_T("Component", "Init", "Already", "already initialized");
        return 0;
    }

    int loaded = 0;
    for (int i = 0; i < g_component_count; i++) {
        component_t *c = g_components[i];
        const char *ver = read_version_file(c->path);
        if (ver) {
            safe_strncpy(g_version_buf[i], ver, sizeof(g_version_buf[i]));
            c->cur_version = g_version_buf[i];
            loaded++;
            LOG_DEBUG_T("Component", "Init", "Loaded", "%s version=%s", c->name, c->cur_version);
        } else {
            c->cur_version = "unknown";
            LOG_WARN_T("Component", "Init", "NoVersion", "%s version unknown", c->name);
        }
    }

    g_initialized = 1;
    LOG_INFO_T("Component", "Init", "OK", "loaded %d/%d components", loaded, g_component_count);
    return 0;
}

const char *component_get_version(const char *name) {
    LOG_DEBUG_T("Component", "GetVersion", "Enter", "name='%s'", name ? name : "(null)");

    if (!name) {
        LOG_ERROR_T("Component", "GetVersion", "Invalid", "name is NULL");
        return NULL;
    }

    for (int i = 0; i < g_component_count; i++) {
        if (strcmp(g_components[i]->name, name) == 0) {
            const char *ver = g_components[i]->cur_version;
            LOG_DEBUG_T("Component", "GetVersion", "OK", "%s -> %s", name, ver);
            return ver;
        }
    }

    LOG_WARN_T("Component", "GetVersion", "NotFound", "component %s not found", name);
    return NULL;
}

int component_is_compatible(const component_t *comp) {
    if (!comp) {
        LOG_ERROR_T("Component", "IsCompatible", "Invalid", "comp is NULL");
        return 0;
    }

    if (!comp->cur_version || strcmp(comp->cur_version, "unknown") == 0) {
        LOG_DEBUG_T("Component", "IsCompatible", "Unknown", "%s version unknown, assuming compatible", comp->name);
        return 1;
    }

    int cur = atoi(comp->cur_version);
    int min = atoi(comp->min_supported);
    int max = atoi(comp->max_supported);

    /* 如果版本号解析失败（非数字），视为不兼容 */
    if (cur == 0 && strcmp(comp->cur_version, "0") != 0) {
        LOG_WARN_T("Component", "IsCompatible", "ParseFail", "%s version '%s' not numeric", comp->name, comp->cur_version);
        return 0;
    }

    int compatible = (cur >= min && cur <= max);
    LOG_DEBUG_T("Component", "IsCompatible", "Result", "%s cur=%d min=%d max=%d -> %s",
                comp->name, cur, min, max, compatible ? "compatible" : "incompatible");
    return compatible;
}

int component_upgrade(const char *name) {
    LOG_INFO_T("Component", "Upgrade", "Enter", "name='%s'", name ? name : "(null)");

    if (!name) {
        LOG_ERROR_T("Component", "Upgrade", "Invalid", "name is NULL");
        return -1;
    }

    for (int i = 0; i < g_component_count; i++) {
        component_t *c = g_components[i];
        if (strcmp(c->name, name) == 0) {
            if (!c->migrate) {
                LOG_WARN_T("Component", "Upgrade", "NoMigrate", "component %s has no migration function", name);
                return -1;
            }

            LOG_INFO_T("Component", "Upgrade", "Start", "upgrading %s from %s", name, c->cur_version);
            int ret = c->migrate();

            if (ret == 0) {
                const char *new_ver = read_version_file(c->path);
                if (new_ver) {
                    safe_strncpy(g_version_buf[i], new_ver, sizeof(g_version_buf[i]));
                    c->cur_version = g_version_buf[i];
                }
                LOG_INFO_T("Component", "Upgrade", "OK", "%s upgraded to %s", name, c->cur_version);
            } else {
                LOG_ERROR_T("Component", "Upgrade", "Fail", "%s upgrade failed with code %d", name, ret);
            }
            return ret;
        }
    }

    LOG_WARN_T("Component", "Upgrade", "NotFound", "component %s not found", name);
    return -1;
}

int component_upgrade_all(void) {
    LOG_INFO_T("Component", "UpgradeAll", "Enter", "checking all components for compatibility");

    int success = 0;
    for (int i = 0; i < g_component_count; i++) {
        component_t *c = g_components[i];

        if (!component_is_compatible(c)) {
            uart_puts(tr("Component ", "组件 "));
            uart_puts(c->name);
            uart_puts(tr(" version mismatch (", " 版本不匹配 ("));
            uart_puts(c->cur_version);
            uart_puts(tr(" needs ", " 需要 "));
            uart_puts(c->min_supported);
            uart_puts("-");
            uart_puts(c->max_supported);
            uart_puts("). ");

            uart_puts(tr("Upgrade? (y/N): ", "升级？(y/N): "));
            char ans = uart_getc();
            uart_putc(ans);
            uart_puts("\n");

            if (ans == 'y' || ans == 'Y') {
                if (component_upgrade(c->name) == 0) {
                    success++;
                }
            } else {
                uart_puts(tr("Skipped.\n", "已跳过。\n"));
            }
        } else {
            LOG_DEBUG_T("Component", "UpgradeAll", "Compatible", "%s version %s is compatible", c->name, c->cur_version);
            success++;
        }
    }

    LOG_INFO_T("Component", "UpgradeAll", "Done", "upgraded %d components", success);
    return success;
}

void component_show_status(void) {
    LOG_DEBUG_T("Component", "ShowStatus", "Enter", "displaying component status");

    uart_puts(tr("\nComponent Versions:\n", "\n组件版本：\n"));

    for (int i = 0; i < g_component_count; i++) {
        component_t *c = g_components[i];
        char buf[256];

        safe_snprintf(buf, sizeof(buf), "  %s: %s", c->name, c->cur_version ? c->cur_version : "unknown");
        uart_puts(buf);

        if (!component_is_compatible(c)) {
            uart_puts(tr(" (incompatible, need ", " (不兼容，需要 "));
            uart_puts(c->min_supported);
            uart_puts("-");
            uart_puts(c->max_supported);
            uart_puts(")");
        }
        uart_puts("\n");

        LOG_DEBUG_T("Component", "ShowStatus", "Item", "%s: %s %s",
                    c->name, c->cur_version ? c->cur_version : "unknown",
                    component_is_compatible(c) ? "(compatible)" : "(incompatible)");
    }
}