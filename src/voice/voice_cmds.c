/**
 * @file    voice_cmds.c
 * @brief   阶段一命令执行（基本反馈）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程（命令解析容错）
 */

#include "voice_cmds.h"
#include "../drivers/uart.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 执行语音命令（阶段一）
 * ============================================================ */

int voice_cmd_execute(const char *cmd) {
    LOG_DEBUG_T("VoiceCmd", "Execute", "Enter", "cmd=%s", cmd ? cmd : "(null)");

    if (!cmd || !*cmd) {
        LOG_WARN_T("VoiceCmd", "Execute", "Empty", "command is empty");
        return -1;
    }

    /* 阶段一：仅反馈（检测到唤醒词后播放提示音） */
    /* 实际可在后续阶段扩展为执行具体命令 */

    /* 【第9期·双语/可行动】原横幅："🎤 Wakeword detected!" 只有英文（双语缺失），
     *   且右边框空格是手写的 → 中文行必然错位；也没有告诉用户"下一步在哪"。
     *   现：中英各一行（tr 成对）、文本行不画右边框（任何语言都不会错位）、
     *   第三行给出可行动路径。 */
    uart_puts("\n");
    uart_puts(COLOR_CYAN);
    uart_puts("┌─────────────────────────────────────────────────────────────┐\n");
    uart_puts("│  🎤  "); uart_puts(tr("Wakeword detected!", "检测到唤醒词！")); uart_puts("\n");
    uart_puts("│  "); uart_puts(tr("I'm here.", "我在。")); uart_puts("\n");
    uart_puts("│  "); uart_puts(tr("Say/type your request in the LING OS app chat.",
                                "请在 LING OS App「AI 对话」里说/输入你的请求。")); uart_puts("\n");
    uart_puts("└─────────────────────────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);

    LOG_INFO_T("VoiceCmd", "Execute", "OK", "feedback shown for '%s'", cmd);
    return 0;
}

/* ============================================================
 * 解析并执行（预留）
 * ============================================================ */

int voice_cmd_parse(const char *text) {
    if (!text) return -1;

    /* 阶段一：仅识别问候 */
    if (strstr(text, "你好") || strstr(text, "hello") || strstr(text, "hi")) {
        return voice_cmd_execute("greeting");
    }

    /* 默认执行 */
    return voice_cmd_execute(text);
}