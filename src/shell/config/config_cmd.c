/**
 * @file    config_cmd.c
 * @brief   配置管理命令实现（set/get/list/rollback/diff）
 * @version LN-B-3.8.0.0
 */

#include "../../wizard/wizard_core.h"
#include "../../common/lang.h"
#include "../../common/data_path.h"
#include "../../lib/log_extra.h"
#include "../../drivers/uart.h"
#include "../../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

/* ============================================================
 * 配置读写辅助
 * ============================================================ */

static char* read_file_content(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

static int write_file_content(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s", content);
    fclose(fp);
    return 0;
}

/* ============================================================
 * 命令实现
 * ============================================================ */

static void cmd_config_set(const char *args) {
    char key[128] = {0};
    char value[512] = {0};

    /* 解析 key=value */
    const char *p = args;
    while (*p == ' ') p++;
    const char *eq = strchr(p, '=');
    if (!eq) {
        uart_puts(tr("Usage: config set <key>=<value>\n", "用法：config set <key>=<value>\n"));
        uart_puts(tr("Example: config set ai.backend=ollama\n", "示例：config set ai.backend=ollama\n"));
        return;
    }

    int key_len = eq - p;
    if (key_len >= (int)sizeof(key)) key_len = sizeof(key) - 1;
    strncpy(key, p, key_len);
    key[key_len] = '\0';

    const char *val = eq + 1;
    strncpy(value, val, sizeof(value) - 1);

    /* 根据 key 前缀决定修改哪个配置文件 */
    const char *root = lingos_data_root();
    char config_path[512];

    if (strncmp(key, "ai.", 3) == 0) {
        snprintf(config_path, sizeof(config_path), "%s/system/config/ai_config.json", root);
        char *content = read_file_content(config_path);
        if (!content) {
            uart_puts(tr("Failed to read AI config\n", "读取 AI 配置失败\n"));
            return;
        }

        /* 简单 JSON 替换（实际应使用 cJSON，此处简化） */
        char *search_str = malloc(strlen(key) + 4);
        sprintf(search_str, "\"%s\"", key + 3);
        char *found = strstr(content, search_str);
        if (found) {
            /* 找到值位置，替换 */
            char *colon = strchr(found, ':');
            if (colon) {
                char *new_content = malloc(strlen(content) + strlen(value) + 64);
                int offset = colon - content + 1;
                strncpy(new_content, content, offset);
                new_content[offset] = '\0';
                if (value[0] != '"' && value[0] != '[' && value[0] != '{' && 
                    !(value[0] >= '0' && value[0] <= '9')) {
                    sprintf(new_content + offset, " \"%s\"", value);
                } else {
                    sprintf(new_content + offset, " %s", value);
                }
                char *end = strchr(colon, ',');
                if (!end) end = strchr(colon, '}');
                if (end) {
                    strcat(new_content, end);
                }
                if (write_file_content(config_path, new_content) == 0) {
                    uart_puts(tr("Configuration updated\n", "配置已更新\n"));
                } else {
                    uart_puts(tr("Failed to write configuration\n", "写入配置失败\n"));
                }
                free(new_content);
            } else {
                uart_puts(tr("Invalid configuration format\n", "配置格式无效\n"));
            }
        } else {
            uart_puts(tr("Key not found in configuration\n", "配置中未找到该键\n"));
        }
        free(search_str);
        free(content);
    } else {
        uart_puts(tr("Only 'ai.*' keys are supported currently\n", "当前仅支持 'ai.*' 键\n"));
    }
}

static void cmd_config_get(const char *key) {
    if (!key || !*key) {
        uart_puts(tr("Usage: config get <key>\n", "用法：config get <key>\n"));
        return;
    }

    const char *root = lingos_data_root();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/system/config/ai_config.json", root);

    char *content = read_file_content(config_path);
    if (!content) {
        uart_puts(tr("Failed to read AI config\n", "读取 AI 配置失败\n"));
        return;
    }

    char search_str[128];
    snprintf(search_str, sizeof(search_str), "\"%s\"", key);
    char *found = strstr(content, search_str);
    if (found) {
        char *colon = strchr(found, ':');
        if (colon) {
            char *start = colon + 1;
            while (*start == ' ') start++;
            char *end = start;
            int depth = 0;
            while (*end) {
                if (*end == '"') {
                    if (depth == 0) {
                        end++;
                        while (*end && *end != '"') end++;
                        if (*end == '"') end++;
                        break;
                    }
                }
                if (*end == '{' || *end == '[') depth++;
                if (*end == '}' || *end == ']') depth--;
                if (depth == 0 && (*end == ',' || *end == '}')) break;
                end++;
            }
            int len = end - start;
            char *value = malloc(len + 1);
            strncpy(value, start, len);
            value[len] = '\0';
            uart_puts(key);
            uart_puts(" = ");
            uart_puts(value);
            uart_puts("\n");
            free(value);
        } else {
            uart_puts(tr("Invalid format\n", "格式无效\n"));
        }
    } else {
        uart_puts(tr("Key not found\n", "键未找到\n"));
    }
    free(content);
}

static void cmd_config_list(void) {
    const char *root = lingos_data_root();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/system/config/ai_config.json", root);

    char *content = read_file_content(config_path);
    if (!content) {
        uart_puts(tr("Failed to read AI config\n", "读取 AI 配置失败\n"));
        return;
    }

    uart_puts(tr("\n=== AI Configuration ===\n", "\n=== AI 配置 ===\n"));
    uart_puts(content);
    uart_puts("\n");
    free(content);
}

static void cmd_config_rollback(void) {
    uart_puts(tr("Configuration rollback: use 'system rollback' for system rollback\n",
                "配置回滚：使用 'system rollback' 进行系统回滚\n"));
    uart_puts(tr("Or manually restore from /LINGOS/backups/\n",
                "或手动从 /LINGOS/backups/ 恢复\n"));
}

static void cmd_config_diff(void) {
    uart_puts(tr("Config diff: use 'system rollback' to view backup list\n",
                "配置差异：使用 'system rollback' 查看备份列表\n"));
}

/* ============================================================
 * 调度入口
 * ============================================================ */

void config_dispatch(const char *args) {
    if (!args || !*args) {
        uart_puts(tr("Usage: config <subcommand> [args]\n", "用法：config <子命令> [参数]\n"));
        uart_puts(tr("Subcommands:\n", "子命令：\n"));
        uart_puts(tr("  set <key>=<value>  - Set configuration value\n", "  set <key>=<value>  - 设置配置值\n"));
        uart_puts(tr("  get <key>          - Get configuration value\n", "  get <key>          - 获取配置值\n"));
        uart_puts(tr("  list               - List all configuration\n", "  list               - 列出所有配置\n"));
        uart_puts(tr("  rollback           - Rollback configuration\n", "  rollback           - 回滚配置\n"));
        uart_puts(tr("  diff               - Show differences\n", "  diff               - 显示差异\n"));
        return;
    }

    char cmd_buf[256];
    strncpy(cmd_buf, args, sizeof(cmd_buf) - 1);
    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *subargs = strtok_r(NULL, "", &saveptr);

    if (!subcmd) return;

    if (strcmp(subcmd, "set") == 0) {
        if (subargs) cmd_config_set(subargs);
        else uart_puts(tr("Usage: config set <key>=<value>\n", "用法：config set <key>=<value>\n"));
    } else if (strcmp(subcmd, "get") == 0) {
        cmd_config_get(subargs ? subargs : "");
    } else if (strcmp(subcmd, "list") == 0) {
        cmd_config_list();
    } else if (strcmp(subcmd, "rollback") == 0) {
        cmd_config_rollback();
    } else if (strcmp(subcmd, "diff") == 0) {
        cmd_config_diff();
    } else {
        uart_puts(tr("Unknown config subcommand\n", "未知的 config 子命令\n"));
    }
}