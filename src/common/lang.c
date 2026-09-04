/**
 * @file    src/common/lang.c
 * @brief   多语言支持实现（流式风格 + 配置记忆 + 系统默认语言检测）
 * @version LN-0.4.3
 * @changes 语言来源改为 config_core_get()，保留文件回退；
 *          新增 lang_set_system_default() 用于根据 LANG 环境变量设置临时语言；
 *          添加 lang_reload() 供配置重载时调用。
 */

#include "lang.h"
#include "uart.h"
#include "log_extra.h"
#include "safe_string.h"
#include "data_path.h"
#include "../config/config_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

lang_t g_language = LANG_EN;
int lingos_force_english = 0;
int _current_lang = 0;          /* 供 ai_master.c 使用的旧式语言变量（0=EN, 1=ZH） */
static int g_lang_initialized = 0;

/* ============================================================
 * 从配置文件读取语言（回退方案）
 * ============================================================ */
int lang_load_from_config(lang_t *out_lang) {
    if (!out_lang) return -1;

    const char *root = lingos_data_root();
    char config_path[512];
    safe_snprintf(config_path, sizeof(config_path), "%s/system/config/ai_config.json", root);

    if (access(config_path, F_OK) != 0) {
        LOG_DEBUG_T("Lang", "LoadConfig", "NoFile", "config file not found");
        return -1;
    }

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_WARN_T("Lang", "LoadConfig", "OpenFail", "cannot open %s", config_path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    char *p = strstr(buf, "\"language\"");
    int found = 0;
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                if (strncmp(p, "zh", 2) == 0 || strncmp(p, "zh-CN", 5) == 0) {
                    *out_lang = LANG_ZH;
                    found = 1;
                } else if (strncmp(p, "en", 2) == 0 || strncmp(p, "en-US", 5) == 0) {
                    *out_lang = LANG_EN;
                    found = 1;
                }
            }
        }
    }
    free(buf);

    if (found) {
        LOG_DEBUG_T("Lang", "LoadConfig", "OK", "language=%s", *out_lang == LANG_ZH ? "ZH" : "EN");
        return 0;
    }
    return -1;
}

/* ============================================================
 * 保存语言到配置文件
 * ============================================================ */
static int lang_save_to_config(lang_t lang) {
    const char *root = lingos_data_root();
    char config_path[512];
    safe_snprintf(config_path, sizeof(config_path), "%s/system/config/ai_config.json", root);

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_WARN_T("Lang", "SaveConfig", "OpenFail", "cannot open %s", config_path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    char new_content[4096];
    const char *lang_str = (lang == LANG_ZH) ? "\"zh-CN\"" : "\"en-US\"";

    char *p = strstr(buf, "\"language\"");
    if (p) {
        char *colon = strchr(p, ':');
        if (colon) {
            char *start = colon + 1;
            while (*start == ' ' || *start == '\t') start++;
            char *end = start;
            if (*end == '"') {
                end++;
                while (*end && *end != '"') end++;
                if (*end == '"') end++;
            } else {
                while (*end && (*end != ',' && *end != '}')) end++;
            }
            size_t prefix_len = start - buf;
            size_t suffix_len = len - (end - buf);
            safe_snprintf(new_content, sizeof(new_content),
                          "%.*s%s%.*s",
                          (int)prefix_len, buf,
                          lang_str,
                          (int)suffix_len, end);
        } else {
            safe_strncpy(new_content, buf, sizeof(new_content));
        }
    } else {
        char *last_brace = strrchr(buf, '}');
        if (last_brace) {
            size_t prefix_len = last_brace - buf;
            safe_snprintf(new_content, sizeof(new_content),
                          "%.*s,\n  \"language\": %s\n}",
                          (int)prefix_len, buf,
                          lang_str);
        } else {
            safe_strncpy(new_content, buf, sizeof(new_content));
        }
    }

    free(buf);

    fp = fopen(config_path, "w");
    if (!fp) {
        LOG_ERROR_T("Lang", "SaveConfig", "WriteFail", "cannot write to %s", config_path);
        return -1;
    }
    fprintf(fp, "%s\n", new_content);
    fclose(fp);

    LOG_INFO_T("Lang", "SaveConfig", "OK", "language saved as %s", lang_str);
    return 0;
}

/* ============================================================
 * 交互式语言选择（流式风格）
 * ============================================================ */
lang_t lang_interactive_select(int force) {
    (void)force;

    uart_puts("\n");
    uart_puts(tr("Starting LING OS", "正在启动 LING OS"));
    uart_puts("\n");
    uart_puts(tr("Welcome to LING OS!", "欢迎使用 LING OS！"));
    uart_puts("\n");

    lang_t saved_lang = LANG_EN;
    int has_saved = (lang_load_from_config(&saved_lang) == 0);

    if (has_saved && !force) {
        g_language = saved_lang;
        _current_lang = (saved_lang == LANG_ZH) ? 1 : 0;
        LOG_INFO_T("Lang", "Init", "FromConfig", "language loaded: %s", g_language == LANG_ZH ? "ZH" : "EN");
        return g_language;
    }

    uart_puts(tr(
        "  [1] English\n"
        "  [2] 中文\n"
        "Please select language (1/2): ",
        "  [1] English\n"
        "  [2] 中文\n"
        "请选择语言 (1/2): "
    ));

    char choice = uart_getc();
    uart_putc(choice);
    uart_puts("\n");

    lang_t selected = LANG_EN;
    if (choice == '2') {
        selected = LANG_ZH;
        uart_puts(tr("Language set to 中文\n", "语言已设为 中文\n"));
    } else {
        selected = LANG_EN;
        uart_puts(tr("Language set to English\n", "语言已设为 English\n"));
    }

    g_language = selected;
    _current_lang = (selected == LANG_ZH) ? 1 : 0;

    lang_save_to_config(selected);

    LOG_INFO_T("Lang", "Init", "Interactive", "language selected: %s", selected == LANG_ZH ? "ZH" : "EN");
    return selected;
}

/* ============================================================
 * FTF[根据系统 LANG 环境变量设置临时语言]
 * ============================================================ */
void lang_set_system_default(void) {
    const char *lang_env = getenv("LANG");
    if (lang_env && strncmp(lang_env, "zh", 2) == 0) {
        g_language = LANG_ZH;
        _current_lang = 1;
        LOG_INFO_T("Lang", "SystemDefault", "OK", "set to ZH (from LANG=%s)", lang_env);
    } else {
        g_language = LANG_EN;
        _current_lang = 0;
        LOG_INFO_T("Lang", "SystemDefault", "OK", "set to EN (from LANG=%s)", lang_env ? lang_env : "(null)");
    }
}

/* ============================================================
 * 语言初始化（主入口）
 * ============================================================ */
int lang_init(void) {
    if (g_lang_initialized) {
        LOG_DEBUG_T("Lang", "Init", "Already", "language already initialized");
        return 0;
    }

    LOG_DEBUG_T("Lang", "Init", "Enter", "initializing language system");

    if (lingos_force_english) {
        g_language = LANG_EN;
        _current_lang = 0;
        g_lang_initialized = 1;
        LOG_INFO_T("Lang", "Init", "ForceEnglish", "forced to English");
        return 0;
    }

    /* 优先从 config_core 读取 */
    /* FF[src/config/config_core.c]-CFN[config_core_get]-FTF[获取配置单例] */
    const wizard_config_t *cfg = config_core_get();
    if (cfg && cfg->language[0] != '\0') {
        const char *lang_str = cfg->language;
        if (strcmp(lang_str, "zh") == 0 || strcmp(lang_str, "zh-CN") == 0) {
            g_language = LANG_ZH;
            _current_lang = 1;
        } else {
            g_language = LANG_EN;
            _current_lang = 0;
        }
        g_lang_initialized = 1;
        LOG_INFO_T("Lang", "Init", "FromConfigCore", "language loaded: %s",
                   g_language == LANG_ZH ? "ZH" : "EN");
        return 0;
    }

    /* 回退到文件读取 */
    lang_t saved_lang = LANG_EN;
    if (lang_load_from_config(&saved_lang) == 0) {
        g_language = saved_lang;
        _current_lang = (saved_lang == LANG_ZH) ? 1 : 0;
        g_lang_initialized = 1;
        LOG_INFO_T("Lang", "Init", "FromConfigFile", "language loaded: %s",
                   g_language == LANG_ZH ? "ZH" : "EN");
        return 0;
    }

    /* 交互式选择 */
    lang_interactive_select(0);
    g_lang_initialized = 1;
    LOG_INFO_T("Lang", "Init", "Interactive", "language initialized: %s",
               g_language == LANG_ZH ? "ZH" : "EN");
    return 0;
}

/* ============================================================
 * FTF[重新加载语言（配置重载时调用）]
 * ============================================================ */
void lang_reload(void) {
    LOG_INFO_T("Lang", "Reload", "Enter", "reloading language from config");
    g_lang_initialized = 0;
    lang_init();
}

/* ============================================================
 * 获取当前语言
 * ============================================================ */
lang_t lang_get_current(void) {
    if (!g_lang_initialized) {
        lang_init();
    }
    return g_language;
}

/* ============================================================
 * 设置语言（并保存）
 * ============================================================ */
int lang_set(lang_t lang) {
    if (lang != LANG_EN && lang != LANG_ZH) {
        LOG_ERROR_T("Lang", "Set", "Invalid", "invalid language value");
        return -1;
    }
    g_language = lang;
    _current_lang = (lang == LANG_ZH) ? 1 : 0;
    if (lang_save_to_config(lang) != 0) {
        LOG_WARN_T("Lang", "Set", "SaveFail", "language set but save failed");
        return -1;
    }
    LOG_INFO_T("Lang", "Set", "OK", "language set to %s", lang == LANG_ZH ? "ZH" : "EN");
    return 0;
}

/* ============================================================
 * 翻译函数
 * ============================================================ */
const char *tr(const char *en, const char *zh) {
    if (g_language == LANG_ZH && zh != NULL) {
        return zh;
    }
    return en ? en : "";
}