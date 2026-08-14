/**
 * @file    component_version.c
 * @brief   组件版本管理（注册、检查兼容性、升级）
 * @version 2.0.0.0
 */

#include "component_version.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "uart.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_COMPONENTS 32

static component_t *components[MAX_COMPONENTS];
static int component_count = 0;
static char version_buf[256][64];

int component_register(component_t *comp) {
    if (component_count >= MAX_COMPONENTS) return -1;
    components[component_count++] = comp;
    LOG_DEBUG_T("Component", "Register", "OK", "registered %s", comp->name);
    return 0;
}

static const char *read_version_file(const char *path) {
    static char version[64];
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    if (fgets(version, sizeof(version), fp)) {
        char *newline = strchr(version, '\n');
        if (newline) *newline = '\0';
        fclose(fp);
        return version;
    }
    fclose(fp);
    return NULL;
}

int component_version_init(void) {
    for (int i = 0; i < component_count; i++) {
        component_t *c = components[i];
        const char *ver = read_version_file(c->path);
        if (ver) {
            strncpy(version_buf[i], ver, sizeof(version_buf[i])-1);
            c->cur_version = version_buf[i];
        } else {
            c->cur_version = "unknown";
        }
        LOG_DEBUG_T("Component", "Init", "Loaded", "%s version=%s", c->name, c->cur_version);
    }
    return 0;
}

const char *component_get_version(const char *name) {
    for (int i = 0; i < component_count; i++) {
        if (strcmp(components[i]->name, name) == 0) {
            return components[i]->cur_version;
        }
    }
    return NULL;
}

int component_is_compatible(const component_t *comp) {
    if (!comp->cur_version || strcmp(comp->cur_version, "unknown") == 0) return 1;
    int cur = atoi(comp->cur_version);
    int min = atoi(comp->min_supported);
    int max = atoi(comp->max_supported);
    return (cur >= min && cur <= max);
}

int component_upgrade(const char *name) {
    for (int i = 0; i < component_count; i++) {
        component_t *c = components[i];
        if (strcmp(c->name, name) == 0) {
            if (!c->migrate) {
                LOG_WARN_T("Component", "Upgrade", "NoMigrate", "component %s has no migration", name);
                return -1;
            }
            int ret = c->migrate();
            if (ret == 0) {
                const char *new_ver = read_version_file(c->path);
                if (new_ver) {
                    strncpy(version_buf[i], new_ver, sizeof(version_buf[i])-1);
                    c->cur_version = version_buf[i];
                }
                LOG_INFO_T("Component", "Upgrade", "OK", "%s upgraded to %s", name, c->cur_version);
            }
            return ret;
        }
    }
    return -1;
}

int component_upgrade_all(void) {
    int success = 0;
    for (int i = 0; i < component_count; i++) {
        component_t *c = components[i];
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
                if (component_upgrade(c->name) == 0) success++;
            } else {
                uart_puts(tr("Skipped.\n", "已跳过。\n"));
            }
        } else {
            success++;
        }
    }
    return success;
}

void component_show_status(void) {
    uart_puts(tr("\nComponent Versions:\n", "\n组件版本：\n"));
    for (int i = 0; i < component_count; i++) {
        component_t *c = components[i];
        char buf[256];
        snprintf(buf, sizeof(buf), "  %s: %s", c->name, c->cur_version);
        uart_puts(buf);
        if (!component_is_compatible(c)) {
            uart_puts(tr(" (incompatible, need ", " (不兼容，需要 "));
            uart_puts(c->min_supported);
            uart_puts("-");
            uart_puts(c->max_supported);
            uart_puts(")");
        }
        uart_puts("\n");
    }
}