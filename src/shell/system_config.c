/**
 * @file    system_config.c
 * @brief   交互式系统配置命令及配置重载命令
 * @version 2.1.0.0
 */

#include "../lib/platform.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../common/mode.h"
#include "../common/version.h"
#include "../fs/fs_layout.h"
#include "../security/uid.h"
#include "../drivers/uart.h"
#include "log_extra.h"
#include "../security/defense.h"
#include "../security/permission.h"
#include "../core/config_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

static const char *get_state_file(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s/Ensystem/state.json", root);
    }
    return path;
}

static void write_state_file(int configured, const char *config_time) {
    const char *path = get_state_file();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        uart_puts(tr("[ERROR] Cannot write state file.\n", "[错误] 无法写入状态文件。\n"));
        return;
    }
    fprintf(fp, "{\n");
    fprintf(fp, "  \"system_configured\": %d,\n", configured);
    fprintf(fp, "  \"last_config_time\": \"%s\",\n", config_time ? config_time : "");
    fprintf(fp, "  \"mode\": \"%s\"\n", lingos_mode_name(lingos_get_mode()));
    fprintf(fp, "}\n");
    fclose(fp);
}

static int read_state_file(int *configured, char *config_time, size_t time_len) {
    const char *path = get_state_file();
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    *configured = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"system_configured\":")) {
            *configured = (strstr(line, "1") != NULL);
        } else if (strstr(line, "\"last_config_time\"")) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '"') p++;
                char *q = p;
                while (*q && *q != '"') q++;
                int len = q - p;
                if (len > 0 && len < (int)time_len - 1) {
                    memcpy(config_time, p, len);
                    config_time[len] = '\0';
                }
            }
        }
    }
    fclose(fp);
    return 0;
}

static int ask_confirmation(const char *prompt_en, const char *prompt_zh) {
    uart_puts(tr(prompt_en, prompt_zh));
    uart_puts(tr(" (y/N): ", " (y/N): "));
    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");
    return (c == 'y' || c == 'Y');
}

static void ask_string(const char *prompt_en, const char *prompt_zh, char *buf, size_t bufsize, const char *default_val) {
    uart_puts(tr(prompt_en, prompt_zh));
    if (default_val && default_val[0]) {
        uart_puts(tr(" [", " ["));
        uart_puts(default_val);
        uart_puts("]");
    }
    uart_puts(tr(": ", ": "));
    int i = 0;
    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') break;
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; uart_puts("\b \b"); }
        } else if (i < (int)bufsize - 1) {
            buf[i++] = c;
            uart_putc(c);
        }
    }
    buf[i] = '\0';
    uart_puts("\n");
    if (i == 0 && default_val) {
        strncpy(buf, default_val, bufsize - 1);
        buf[bufsize-1] = '\0';
    }
}

static void set_root_password(void) {
    char pass1[128] = {0}, pass2[128] = {0};
    ask_string("Set root password (leave empty for no password)",
               "设置 root 密码（留空则无密码）",
               pass1, sizeof(pass1), "");
    if (strlen(pass1) == 0) {
        uart_puts(tr("Root password will be empty.\n", "root 密码将为空。\n"));
        return;
    }
    ask_string("Confirm password", "确认密码", pass2, sizeof(pass2), "");
    if (strcmp(pass1, pass2) != 0) {
        uart_puts(tr("Passwords do not match. Root password not set.\n", "密码不匹配，未设置 root 密码。\n"));
        return;
    }
    const char *root = lingos_data_root();
    char passwd_path[512];
    snprintf(passwd_path, sizeof(passwd_path), "%s/Ensystem/passwd", root);
    FILE *fp = fopen(passwd_path, "w");
    if (fp) {
        fprintf(fp, "root:%s\n", pass1);
        fclose(fp);
        uart_puts(tr("Root password set.\n", "root 密码已设置。\n"));
    } else {
        uart_puts(tr("Failed to save password.\n", "保存密码失败。\n"));
    }
}

static int check_root_password(void) {
    const char *root = lingos_data_root();
    char passwd_path[512];
    snprintf(passwd_path, sizeof(passwd_path), "%s/Ensystem/passwd", root);
    FILE *fp = fopen(passwd_path, "r");
    if (!fp) return 0;
    char line[256];
    int has_password = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "root:", 5) == 0) {
            char *p = line + 5;
            while (*p && *p != '\n') p++;
            if (p - (line+5) > 1) has_password = 1;
            break;
        }
    }
    fclose(fp);
    return has_password;
}

void config_reload_command(void) {
    uart_puts(tr("Reloading configurations...\n", "正在重载配置...\n"));
    int ret = config_reload_all();
    if (ret == 0) {
        uart_puts(tr("All configurations reloaded successfully.\n", "所有配置已成功重载。\n"));
    } else {
        uart_puts(tr("Some configurations failed to reload. Check logs.\n", "部分配置重载失败，请查看日志。\n"));
    }
}

void system_configuration_command(void) {
    uart_puts(tr("\n=== LING OS System Configuration ===\n", "\n=== LING OS 系统配置 ===\n"));

    lingos_mode_t mode = lingos_get_mode();
    uart_puts(tr("Current mode: ", "当前模式: "));
    uart_puts(lingos_mode_name(mode));
    uart_puts("\n");

    int already_configured = 0;
    char old_time[64] = {0};
    if (read_state_file(&already_configured, old_time, sizeof(old_time)) == 0 && already_configured) {
        if (!ask_confirmation("System already configured. Reconfigure? (y/N)",
                              "系统已配置。重新配置？(y/N)")) {
            uart_puts(tr("Configuration cancelled.\n", "配置已取消。\n"));
            return;
        }
    }

    char timezone[64] = "Asia/Shanghai";
    ask_string("Time zone (e.g., Asia/Shanghai)", "时区（例如 Asia/Shanghai）",
               timezone, sizeof(timezone), timezone);

    set_root_password();

    uart_puts(tr("AI backend will use Ollama (local). You can change in /LINGOS/system/config/ai_config.json\n",
                 "AI 后端将使用 Ollama（本地）。您可以在 /LINGOS/system/config/ai_config.json 中更改。\n"));

    if (!ask_confirmation("Proceed with configuration? (y/N)", "继续配置？(y/N)")) {
        uart_puts(tr("Configuration cancelled.\n", "配置已取消。\n"));
        return;
    }

    uart_puts(tr("Creating directory structure...\n", "创建目录结构...\n"));
    do_create_layout();

    if (!lingos_mode_config_valid()) {
        lingos_set_mode(mode);
    }

    const char *root = lingos_data_root();
    char tz_path[512];
    snprintf(tz_path, sizeof(tz_path), "%s/system/config/timezone", root);
    FILE *fp = fopen(tz_path, "w");
    if (fp) {
        fprintf(fp, "%s\n", timezone);
        fclose(fp);
    }

    char time_str[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    write_state_file(1, time_str);

    uart_puts(tr("\nConfiguration completed!\n", "\n配置完成！\n"));
    if (!check_root_password()) {
        uart_puts(tr("WARNING: root has no password. Set it with 'passwd' after login.\n",
                     "警告：root 没有密码，登录后请使用 'passwd' 设置。\n"));
    }
}