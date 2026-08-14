/**
 * @file    cmd_plugin.c
 * @brief   插件管理命令：list, load, unload
 * @version LN-B-4.2.0.0
 * @path    src/shell/cmd_plugin.c
 */

#include "plugin.h"
#include "plugin_loader.h"
#include "lang.h"
#include "safe_string.h"
#include "uart.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 内部辅助：显示插件列表
 * ============================================================ */
static void cmd_plugin_list(void) {
    LOG_DEBUG_T("CmdPlugin", "List", "Enter", "listing plugins");

    plugin_t *plugins[64];
    int count = plugin_list(plugins, 64);

    if (count == 0) {
        uart_puts(tr("No plugins loaded.\n", "没有已加载的插件。\n"));
        uart_puts(tr("  Place .so files in /LINGOS/plugins/c/ to load.\n",
                     "  将 .so 文件放入 /LINGOS/plugins/c/ 目录加载。\n"));
        return;
    }

    uart_puts(tr("\n=== Loaded Plugins ===\n", "\n=== 已加载插件 ===\n"));
    char buf[256];

    for (int i = 0; i < count; i++) {
        plugin_t *p = plugins[i];
        safe_snprintf(buf, sizeof(buf),
                      "  %d. %s v%s (%s) - %s\n"
                      "     Type: %s | State: %s\n",
                      i + 1,
                      p->name[0] ? p->name : "(unnamed)",
                      p->version[0] ? p->version : "0.0.0",
                      p->author[0] ? p->author : "unknown",
                      p->description[0] ? p->description : "No description",
                      plugin_type_str(p->type),
                      plugin_state_str(p->state));
        uart_puts(buf);
    }

    safe_snprintf(buf, sizeof(buf), tr("\nTotal: %d plugins\n", "\n总计：%d 个插件\n"), count);
    uart_puts(buf);
    LOG_DEBUG_T("CmdPlugin", "List", "Done", "displayed %d plugins", count);
}

/* ============================================================
 * 内部辅助：加载插件
 * ============================================================ */
static void cmd_plugin_load(const char *path) {
    LOG_INFO_T("CmdPlugin", "Load", "Enter", "path='%s'", path ? path : "(null)");

    if (!path || !*path) {
        uart_puts(tr("Usage: plugin load <path>\n", "用法：plugin load <路径>\n"));
        uart_puts(tr("  Load a plugin from a .so file.\n", "  从 .so 文件加载插件。\n"));
        uart_puts(tr("  Example: plugin load /LINGOS/plugins/c/my_plugin.so\n",
                     "  示例：plugin load /LINGOS/plugins/c/my_plugin.so\n"));
        return;
    }

    if (access(path, F_OK) != 0) {
        uart_puts(tr("Plugin file not found: ", "插件文件未找到："));
        uart_puts(path);
        uart_puts("\n");
        LOG_WARN_T("CmdPlugin", "Load", "NotFound", "%s not found", path);
        return;
    }

    int ret = plugin_loader_load(path);
    if (ret == 0) {
        uart_puts(tr("Plugin loaded successfully.\n", "插件加载成功。\n"));
        LOG_INFO_T("CmdPlugin", "Load", "OK", "loaded %s", path);
    } else {
        uart_puts(tr("Failed to load plugin.\n", "加载插件失败。\n"));
        uart_puts(tr("  Check: Is it a valid .so with plugin_entry()?\n",
                     "  检查：是否为有效的 .so 文件且包含 plugin_entry()？\n"));
        LOG_ERROR_T("CmdPlugin", "Load", "Fail", "failed to load %s", path);
    }
}

/* ============================================================
 * 内部辅助：卸载插件
 * ============================================================ */
static void cmd_plugin_unload(const char *name) {
    LOG_INFO_T("CmdPlugin", "Unload", "Enter", "name='%s'", name ? name : "(null)");

    if (!name || !*name) {
        uart_puts(tr("Usage: plugin unload <name>\n", "用法：plugin unload <名称>\n"));
        uart_puts(tr("  Unload a plugin by its name.\n", "  按名称卸载插件。\n"));
        uart_puts(tr("  Use 'plugin list' to see loaded plugins.\n",
                     "  使用 'plugin list' 查看已加载插件。\n"));
        return;
    }

    plugin_t *p = plugin_find(name);
    if (!p) {
        uart_puts(tr("Plugin not found: ", "插件未找到："));
        uart_puts(name);
        uart_puts("\n");
        LOG_WARN_T("CmdPlugin", "Unload", "NotFound", "%s not found", name);
        return;
    }

    int ret = plugin_unregister(name);
    if (ret == 0) {
        uart_puts(tr("Plugin unloaded: ", "插件已卸载："));
        uart_puts(name);
        uart_puts("\n");
        LOG_INFO_T("CmdPlugin", "Unload", "OK", "unloaded %s", name);
    } else {
        uart_puts(tr("Failed to unload plugin.\n", "卸载插件失败。\n"));
        LOG_ERROR_T("CmdPlugin", "Unload", "Fail", "failed to unload %s", name);
    }
}

/* ============================================================
 * 内部辅助：显示帮助
 * ============================================================ */
static void cmd_plugin_help(void) {
    uart_puts(tr("\nPlugin Commands:\n", "\n插件命令：\n"));
    uart_puts(tr("  plugin list             - List all loaded plugins\n",
                 "  plugin list             - 列出所有已加载插件\n"));
    uart_puts(tr("  plugin load <path>      - Load a plugin from .so file\n",
                 "  plugin load <路径>      - 从 .so 文件加载插件\n"));
    uart_puts(tr("  plugin unload <name>    - Unload a plugin by name\n",
                 "  plugin unload <名称>    - 按名称卸载插件\n"));
    uart_puts(tr("\nPlugin Directory: /LINGOS/plugins/c/\n",
                 "\n插件目录：/LINGOS/plugins/c/\n"));
}

/* ============================================================
 * 公共 API：插件命令分发
 * ============================================================ */
void plugin_dispatch(const char *args) {
    LOG_DEBUG_T("CmdPlugin", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        cmd_plugin_help();
        return;
    }

    char cmd_buf[128];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *subarg = strtok_r(NULL, "", &saveptr);

    if (!subcmd) {
        cmd_plugin_help();
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        cmd_plugin_list();
    } else if (strcmp(subcmd, "load") == 0) {
        if (subarg) {
            while (*subarg == ' ') subarg++;
            cmd_plugin_load(subarg);
        } else {
            uart_puts(tr("Usage: plugin load <path>\n", "用法：plugin load <路径>\n"));
        }
    } else if (strcmp(subcmd, "unload") == 0) {
        if (subarg) {
            while (*subarg == ' ') subarg++;
            cmd_plugin_unload(subarg);
        } else {
            uart_puts(tr("Usage: plugin unload <name>\n", "用法：plugin unload <名称>\n"));
        }
    } else if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0) {
        cmd_plugin_help();
    } else {
        uart_puts(tr("Unknown plugin command: ", "未知插件命令："));
        uart_puts(subcmd);
        uart_puts("\n");
        uart_puts(tr("Available: list, load, unload, help\n",
                     "可用命令：list, load, unload, help\n"));
    }

    LOG_DEBUG_T("CmdPlugin", "Dispatch", "Exit", "subcmd='%s'", subcmd ? subcmd : "(null)");
}