/**
 * @file    config_wizard_common.c
 * @brief   配置向导公共函数（绘制选项、键盘控制、输入框）
 * @version LN-B-4.2.0.0
 */

#include "config_wizard_common.h"
#include "../../common/lang.h"
#include "../../common/safe_string.h"
#include "../../drivers/uart.h"
#include "../../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ============================================================
 * 辅助：绘制选项列表（支持键盘导航）
 * ============================================================ */

void wizard_draw_options(wizard_context_t *ctx) {
    LOG_DEBUG_T("WizardCommon", "DrawOptions", "Enter", "count=%d, focus=%d",
                ctx ? ctx->option_count : -1, ctx ? ctx->focus_index : -1);

    if (!ctx) {
        LOG_ERROR_T("WizardCommon", "DrawOptions", "Invalid", "ctx is NULL");
        return;
    }

    for (int i = 0; i < ctx->option_count; i++) {
        wizard_option_t *opt = &ctx->options[i];
        const char *label = wizard_opt_label(ctx, opt);
        const char *desc = wizard_opt_desc(ctx, opt);
        int is_focused = (i == ctx->focus_index);

        /* 缩进 */
        uart_puts("  ");

        /* 前缀符号 */
        if (is_focused) {
            uart_puts(COLOR_BOLD COLOR_CYAN);
            uart_puts("▶ ");
            uart_puts(COLOR_RESET);
        } else if (opt->state == OPT_STATE_SELECTED) {
            uart_puts(COLOR_GREEN);
            uart_puts("✓ ");
            uart_puts(COLOR_RESET);
        } else if (opt->state == OPT_STATE_DISABLED) {
            uart_puts(COLOR_DIM);
            uart_puts("○ ");
            uart_puts(COLOR_RESET);
        } else {
            uart_puts("  ");
        }

        /* 标签 */
        if (is_focused) {
            uart_puts(COLOR_BOLD COLOR_CYAN);
        } else if (opt->state == OPT_STATE_DISABLED) {
            uart_puts(COLOR_DIM);
        } else {
            uart_puts(COLOR_RESET);
        }
        uart_puts(label);
        uart_puts(COLOR_RESET);

        /* 多选状态 */
        if (opt->is_multi_select && opt->is_selected) {
            uart_puts(COLOR_GREEN);
            uart_puts(" ✓");
            uart_puts(COLOR_RESET);
        }

        /* 描述（仅在焦点时显示） */
        if (is_focused && desc && desc[0]) {
            uart_puts(COLOR_DIM);
            uart_puts(" — ");
            uart_puts(desc);
            uart_puts(COLOR_RESET);
        }

        uart_puts("\n");
    }

    /* 底部操作提示 */
    uart_puts(COLOR_DIM);
    uart_puts(tr("  ↑/↓: navigate  Space: toggle  Enter: confirm  ESC: cancel\n",
                 "  ↑/↓: 导航  Space: 切换  Enter: 确认  ESC: 取消\n"));
    uart_puts(COLOR_RESET);
    LOG_DEBUG_T("WizardCommon", "DrawOptions", "Exit", "options displayed");
}

/* ============================================================
 * 辅助：绘制带输入框的选项（支持输入校验）
 * ============================================================ */

void wizard_draw_input(wizard_context_t *ctx, const char *prompt,
                       const char *value, const char *error_msg) {
    LOG_DEBUG_T("WizardCommon", "DrawInput", "Enter", "prompt='%s', value='%s'",
                prompt ? prompt : "(null)", value ? value : "(null)");

    if (!ctx) {
        LOG_ERROR_T("WizardCommon", "DrawInput", "Invalid", "ctx is NULL");
        return;
    }

    uart_puts(COLOR_CYAN);
    uart_puts(prompt);
    uart_puts(COLOR_RESET);
    uart_puts(": ");

    if (value && value[0]) {
        uart_puts(value);
    } else {
        uart_puts(COLOR_DIM);
        uart_puts(tr("(empty)", "(空)"));
        uart_puts(COLOR_RESET);
    }

    if (error_msg && error_msg[0]) {
        uart_puts(COLOR_RED);
        uart_puts(" ⚠ ");
        uart_puts(error_msg);
        uart_puts(COLOR_RESET);
    }

    uart_puts("\n");
    uart_puts(COLOR_DIM);
    uart_puts(tr("  Enter: submit  ESC: cancel\n", "  Enter: 提交  ESC: 取消\n"));
    uart_puts(COLOR_RESET);

    LOG_DEBUG_T("WizardCommon", "DrawInput", "Exit", "input displayed");
}

/* ============================================================
 * 辅助：读取键盘输入（支持方向键）
 * ============================================================ */

int wizard_read_key(void) {
    LOG_DEBUG_T("WizardCommon", "ReadKey", "Enter", "reading key");

    char c = uart_getc();
    int key = (int)(unsigned char)c;

    LOG_DEBUG_T("WizardCommon", "ReadKey", "Char", "key=%d (0x%x)", key, key);

    /* ESC 序列处理（方向键） */
    if (key == 27) {
        char c2 = uart_getc();
        if (c2 == 0) return 27;  /* 单独 ESC */

        if (c2 == '[' || c2 == 'O') {
            char c3 = uart_getc();
            if (c3 == 0) return 27;

            switch (c3) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                default: return 27;  /* 未知 ESC 序列，当作 ESC */
            }
        }
        return 27;
    }

    /* 普通字符 */
    return key;
}

/* ============================================================
 * 辅助：读取字符串输入（带退格支持）
 * ============================================================ */

void wizard_read_string(char *buf, size_t buf_size) {
    LOG_DEBUG_T("WizardCommon", "ReadString", "Enter", "buf_size=%zu", buf_size);

    if (!buf || buf_size == 0) {
        LOG_ERROR_T("WizardCommon", "ReadString", "Invalid", "buf=%p, buf_size=%zu", (void*)buf, buf_size);
        return;
    }

    size_t idx = 0;
    buf[0] = '\0';

    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            buf[idx] = '\0';
            LOG_DEBUG_T("WizardCommon", "ReadString", "OK", "result='%s'", buf);
            return;
        }
        if (c == 27) {  /* ESC */
            uart_puts("\n");
            buf[0] = '\0';
            LOG_DEBUG_T("WizardCommon", "ReadString", "Cancel", "user cancelled");
            return;
        }
        if (c == '\b' || c == 127) {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
            continue;
        }
        if (idx < buf_size - 1 && c >= 32 && c <= 126) {
            buf[idx++] = c;
            buf[idx] = '\0';
            uart_putc(c);
        }
    }
}

/* ============================================================
 * 辅助：读取密码（不回显）
 * ============================================================ */

void wizard_read_password(char *buf, size_t buf_size) {
    LOG_DEBUG_T("WizardCommon", "ReadPassword", "Enter", "buf_size=%zu", buf_size);

    if (!buf || buf_size == 0) {
        LOG_ERROR_T("WizardCommon", "ReadPassword", "Invalid", "buf=%p, buf_size=%zu", (void*)buf, buf_size);
        return;
    }

    size_t idx = 0;
    buf[0] = '\0';

    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            buf[idx] = '\0';
            LOG_DEBUG_T("WizardCommon", "ReadPassword", "OK", "password read (len=%zu)", idx);
            return;
        }
        if (c == 27) {  /* ESC */
            uart_puts("\n");
            buf[0] = '\0';
            LOG_DEBUG_T("WizardCommon", "ReadPassword", "Cancel", "user cancelled");
            return;
        }
        if (c == '\b' || c == 127) {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
            continue;
        }
        if (idx < buf_size - 1 && c >= 32 && c <= 126) {
            buf[idx++] = c;
            buf[idx] = '\0';
            uart_putc('*');
        }
    }
}

/* ============================================================
 * 辅助：输入校验
 * ============================================================ */

int wizard_validate_deepseek_api_key(const char *key, char *error_msg, size_t msg_len) {
    LOG_DEBUG_T("WizardCommon", "ValidateAPIKey", "Enter", "key='%s'", key ? key : "(null)");

    if (!key || !key[0]) {
        safe_snprintf(error_msg, msg_len, tr("API Key cannot be empty", "API Key 不能为空"));
        LOG_DEBUG_T("WizardCommon", "ValidateAPIKey", "Empty", "key is empty");
        return 0;
    }

    if (strncmp(key, "sk-", 3) != 0) {
        safe_snprintf(error_msg, msg_len, tr("API Key must start with 'sk-'", "API Key 必须以 'sk-' 开头"));
        LOG_DEBUG_T("WizardCommon", "ValidateAPIKey", "Invalid", "key does not start with sk-");
        return 0;
    }

    if (strlen(key) < 10) {
        safe_snprintf(error_msg, msg_len, tr("API Key is too short", "API Key 太短"));
        LOG_DEBUG_T("WizardCommon", "ValidateAPIKey", "TooShort", "key length=%zu", strlen(key));
        return 0;
    }

    LOG_DEBUG_T("WizardCommon", "ValidateAPIKey", "OK", "key is valid");
    return 1;
}

int wizard_validate_url(const char *url, char *error_msg, size_t msg_len) {
    LOG_DEBUG_T("WizardCommon", "ValidateURL", "Enter", "url='%s'", url ? url : "(null)");

    if (!url || !url[0]) {
        safe_snprintf(error_msg, msg_len, tr("URL cannot be empty", "URL 不能为空"));
        LOG_DEBUG_T("WizardCommon", "ValidateURL", "Empty", "url is empty");
        return 0;
    }

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        safe_snprintf(error_msg, msg_len, tr("URL must start with http:// or https://", "URL 必须以 http:// 或 https:// 开头"));
        LOG_DEBUG_T("WizardCommon", "ValidateURL", "Invalid", "url does not start with http:// or https://");
        return 0;
    }

    LOG_DEBUG_T("WizardCommon", "ValidateURL", "OK", "url is valid");
    return 1;
}

int wizard_validate_model_name(const char *model, char *error_msg, size_t msg_len) {
    LOG_DEBUG_T("WizardCommon", "ValidateModel", "Enter", "model='%s'", model ? model : "(null)");

    if (!model || !model[0]) {
        safe_snprintf(error_msg, msg_len, tr("Model name cannot be empty", "模型名称不能为空"));
        LOG_DEBUG_T("WizardCommon", "ValidateModel", "Empty", "model is empty");
        return 0;
    }

    /* 只包含字母、数字、点、下划线、连字符 */
    for (const char *p = model; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != ':') {
            safe_snprintf(error_msg, msg_len,
                          tr("Model name contains invalid characters (only letters, numbers, ., _, -, :)",
                             "模型名称包含无效字符（只允许字母、数字、.、_、-、:）"));
            LOG_DEBUG_T("WizardCommon", "ValidateModel", "Invalid", "invalid char: '%c'", *p);
            return 0;
        }
    }

    LOG_DEBUG_T("WizardCommon", "ValidateModel", "OK", "model is valid");
    return 1;
}

/* ============================================================
 * 辅助：获取选项标签/描述（支持多语言）
 * ============================================================ */

const char* wizard_opt_label(wizard_context_t *ctx, wizard_option_t *opt) {
    if (!ctx || !opt) {
        LOG_WARN_T("WizardCommon", "OptLabel", "Invalid", "ctx=%p, opt=%p", (void*)ctx, (void*)opt);
        return "";
    }

    if (strcmp(ctx->language, "zh") == 0) {
        return opt->label_zh[0] ? opt->label_zh : opt->label_en;
    }
    return opt->label_en;
}

const char* wizard_opt_desc(wizard_context_t *ctx, wizard_option_t *opt) {
    if (!ctx || !opt) {
        LOG_WARN_T("WizardCommon", "OptDesc", "Invalid", "ctx=%p, opt=%p", (void*)ctx, (void*)opt);
        return "";
    }

    if (strcmp(ctx->language, "zh") == 0) {
        return opt->desc_zh[0] ? opt->desc_zh : opt->desc_en;
    }
    return opt->desc_en;
}

/* ============================================================
 * 辅助：确认对话框
 * ============================================================ */

int wizard_confirm(const char *message_en, const char *message_zh) {
    LOG_DEBUG_T("WizardCommon", "Confirm", "Enter", "message_en='%s'", message_en ? message_en : "(null)");

    uart_puts(COLOR_YELLOW);
    uart_puts(tr(message_en, message_zh));
    uart_puts(tr(" (y/N): ", " (y/N): "));
    uart_puts(COLOR_RESET);

    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");

    int result = (c == 'y' || c == 'Y');
    LOG_DEBUG_T("WizardCommon", "Confirm", "Result", "result=%d", result);
    return result;
}