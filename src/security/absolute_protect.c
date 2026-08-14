/**
 * @file    absolute_protect.c
 * @brief   绝对保护模式核心实现
 * @version LN-B-5.0.0.0
 * @fix     自动关闭时使用 defense_mode_set_force() 绕过降级检查
 */

#include "defense_mode.h"
#include "security_config.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* 外部函数声明 */
extern int defense_mode_set_force(defense_mode_t mode);

static pthread_mutex_t g_abs_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_absolute_active = 0;
static volatile time_t g_absolute_activated_at = 0;
static volatile int g_absolute_auto_close_pending = 0;

int absolute_protect_trigger(const char *source) {
    LOG_WARN_T("AbsoluteProtect", "Trigger", "Enter", "source='%s'", source ? source : "(null)");

    pthread_mutex_lock(&g_abs_lock);

    if (g_absolute_active) {
        pthread_mutex_unlock(&g_abs_lock);
        LOG_WARN_T("AbsoluteProtect", "Trigger", "AlreadyActive", "absolute protect already active");
        return 0;
    }

    g_absolute_active = 1;
    g_absolute_activated_at = time(NULL);
    g_absolute_auto_close_pending = 0;

    pthread_mutex_unlock(&g_abs_lock);

    defense_mode_set(DEFENSE_MODE_ABSOLUTE);
    log_set_level(LOG_LEVEL_DEBUG);

    uart_puts(COLOR_RED);
    uart_puts(tr(
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║  ⚠️  ABSOLUTE PROTECTION ACTIVATED                            ║\n"
        "║  All external inputs are blocked.                              ║\n"
        "║  Only system-essential functions are available.               ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n",
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║  ⚠️  绝对保护模式已激活                                        ║\n"
        "║  所有外部输入已被阻断。                                         ║\n"
        "║  仅系统必需功能可用。                                           ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n"
    ));
    uart_puts(COLOR_RESET);

    LOG_WARN_T("AbsoluteProtect", "Trigger", "OK", "absolute protect activated by %s", source ? source : "unknown");
    return 0;
}

int absolute_protect_is_active(void) {
    return g_absolute_active;
}

int absolute_protect_should_auto_close(void) {
    return g_absolute_auto_close_pending;
}

int absolute_protect_auto_close(void) {
    LOG_INFO_T("AbsoluteProtect", "AutoClose", "Enter", "auto closing absolute protect");

    pthread_mutex_lock(&g_abs_lock);

    if (!g_absolute_active) {
        pthread_mutex_unlock(&g_abs_lock);
        LOG_WARN_T("AbsoluteProtect", "AutoClose", "NotActive", "absolute protect not active");
        return -1;
    }

    g_absolute_active = 0;
    g_absolute_activated_at = 0;
    g_absolute_auto_close_pending = 0;

    pthread_mutex_unlock(&g_abs_lock);

    /* 使用强制降级到暗影模式（绕过降级检查） */
    defense_mode_set_force(DEFENSE_MODE_DARK);

    /* 恢复日志级别 */
    log_set_level(LOG_LEVEL_INFO);

    uart_puts(COLOR_GREEN);
    uart_puts(tr(
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║  ✅  ABSOLUTE PROTECTION CLOSED                               ║\n"
        "║  System is now in Dark Mode.                                  ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n",
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║  ✅  绝对保护模式已关闭                                        ║\n"
        "║  系统现已进入暗影模式。                                         ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n"
    ));
    uart_puts(COLOR_RESET);

    LOG_INFO_T("AbsoluteProtect", "AutoClose", "OK", "absolute protect closed, switched to dark mode");
    return 0;
}

void absolute_protect_set_auto_close_pending(void) {
    g_absolute_auto_close_pending = 1;
    LOG_DEBUG_T("AbsoluteProtect", "SetPending", "OK", "auto close pending");
}

int absolute_protect_should_block_input(const char *source) {
    if (!g_absolute_active) return 0;

    if (source && strstr(source, "alert") != NULL) {
        return 0;
    }

    if (source && strstr(source, "ai") != NULL) {
        return 0;
    }

    const char *allowed_sources[] = {"system", "exit", "help", "logdump", NULL};
    for (int i = 0; allowed_sources[i]; i++) {
        if (source && strstr(source, allowed_sources[i]) != NULL) {
            return 0;
        }
    }

    LOG_DEBUG_T("AbsoluteProtect", "BlockInput", "Blocked", "source='%s' blocked", source ? source : "(null)");
    return 1;
}