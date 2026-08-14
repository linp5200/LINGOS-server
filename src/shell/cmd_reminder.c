/**
 * @file    cmd_reminder.c
 * @brief   提醒管理命令：add, list, delete
 * @version LN-B-4.2.0.0
 * @path    src/shell/cmd_reminder.c
 * @note    所有 Shell 命令源文件均置于 src/shell/ 目录下
 */

#include "ai_reminder.h"
#include "lang.h"
#include "safe_string.h"
#include "uart.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ============================================================
 * 内部辅助：解析时间字符串
 * ============================================================ */

static time_t parse_time(const char *str) {
    if (!str || !*str) return 0;

    /* 尝试解析为绝对时间 YYYY-MM-DD HH:MM:SS */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (strptime(str, "%Y-%m-%d %H:%M:%S", &tm) != NULL) {
        return mktime(&tm);
    }

    /* 尝试解析为相对时间 +N (分钟) */
    if (str[0] == '+') {
        int minutes = atoi(str + 1);
        if (minutes > 0) {
            return time(NULL) + minutes * 60;
        }
    }

    /* 尝试解析为自然语言（简单处理） */
    if (strcmp(str, "now") == 0 || strcmp(str, "today") == 0) {
        return time(NULL);
    }

    if (strcmp(str, "tomorrow") == 0) {
        return time(NULL) + 86400;
    }

    /* 无法解析，返回当前时间 + 1 小时 */
    LOG_WARN_T("CmdReminder", "ParseTime", "Unknown", "cannot parse '%s', using now+1h", str);
    return time(NULL) + 3600;
}

/* ============================================================
 * 命令实现
 * ============================================================ */

static void cmd_reminder_add(const char *content, const char *time_str, int repeat, int interval) {
    LOG_INFO_T("CmdReminder", "Add", "Enter", "content='%s', time='%s', repeat=%d, interval=%d",
               content ? content : "(null)", time_str ? time_str : "(null)", repeat, interval);

    if (!content || !*content) {
        uart_puts(tr("Usage: remind add \"<content>\" <time> [repeat]\n",
                     "用法：remind add \"<内容>\" <时间> [重复次数]\n"));
        uart_puts(tr("  Time formats:\n", "  时间格式：\n"));
        uart_puts(tr("    YYYY-MM-DD HH:MM:SS  - Absolute time\n",
                     "    YYYY-MM-DD HH:MM:SS  - 绝对时间\n"));
        uart_puts(tr("    +N                   - N minutes from now\n",
                     "    +N                   - N 分钟后\n"));
        uart_puts(tr("    now                  - Immediately\n",
                     "    now                  - 立即\n"));
        uart_puts(tr("    tomorrow             - Tomorrow at same time\n",
                     "    tomorrow             - 明天同一时间\n"));
        uart_puts(tr("  Example: remind add \"Meeting\" +30\n",
                     "  示例：remind add \"会议\" +30\n"));
        return;
    }

    if (!time_str || !*time_str) {
        uart_puts(tr("Please specify a time.\n", "请指定时间。\n"));
        return;
    }

    time_t trigger_time = parse_time(time_str);
    if (trigger_time == 0) {
        uart_puts(tr("Invalid time format.\n", "无效的时间格式。\n"));
        return;
    }

    /* 初始化提醒系统 */
    reminder_init();

    char id[64];
    int ret = reminder_add(content, trigger_time, repeat, interval, NULL, id, sizeof(id));

    if (ret == 0) {
        char time_buf[64];
        struct tm *tm = localtime(&trigger_time);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);

        uart_puts(tr("Reminder added.\n", "提醒已添加。\n"));
        uart_puts(tr("  ID: ", "  ID: "));
        uart_puts(id);
        uart_puts("\n");
        uart_puts(tr("  Time: ", "  时间："));
        uart_puts(time_buf);
        uart_puts("\n");
        uart_puts(tr("  Content: ", "  内容："));
        uart_puts(content);
        uart_puts("\n");
        if (repeat > 0) {
            char buf[32];
            safe_snprintf(buf, sizeof(buf), tr("  Repeat: %d times, interval %d seconds\n",
                                               "  重复：%d 次，间隔 %d 秒\n"),
                          repeat, interval);
            uart_puts(buf);
        }
        LOG_INFO_T("CmdReminder", "Add", "OK", "added reminder %s", id);
    } else {
        uart_puts(tr("Failed to add reminder.\n", "添加提醒失败。\n"));
        LOG_ERROR_T("CmdReminder", "Add", "Fail", "reminder_add returned %d", ret);
    }
}

static void cmd_reminder_list(void) {
    LOG_DEBUG_T("CmdReminder", "List", "Enter", "listing reminders");

    reminder_init();

    reminder_t reminders[64];
    int count = reminder_list(reminders, 64, 0);  /* 只显示 pending */

    if (count <= 0) {
        uart_puts(tr("No pending reminders.\n", "没有待触发的提醒。\n"));
        return;
    }

    uart_puts(tr("\n=== Pending Reminders ===\n", "\n=== 待触发提醒 ===\n"));
    char buf[256];

    for (int i = 0; i < count; i++) {
        char time_buf[32];
        struct tm *tm = localtime(&reminders[i].trigger_time);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);

        char status_buf[16];
        safe_strncpy(status_buf, reminder_status_name(reminders[i].status), sizeof(status_buf));

        safe_snprintf(buf, sizeof(buf),
                      "  %d. %s\n"
                      "     ID: %s\n"
                      "     Time: %s\n"
                      "     Status: %s\n"
                      "     Repeat: %d\n",
                      i + 1,
                      reminders[i].content,
                      reminders[i].id,
                      time_buf,
                      status_buf,
                      reminders[i].repeat);
        uart_puts(buf);
    }

    safe_snprintf(buf, sizeof(buf), tr("\nTotal: %d pending reminders\n", "\n总计：%d 个待触发提醒\n"), count);
    uart_puts(buf);
}

static void cmd_reminder_delete(const char *id) {
    LOG_INFO_T("CmdReminder", "Delete", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        uart_puts(tr("Usage: remind delete <id>\n", "用法：remind delete <ID>\n"));
        uart_puts(tr("  Use 'remind list' to see reminder IDs.\n",
                     "  使用 'remind list' 查看提醒 ID。\n"));
        return;
    }

    reminder_init();

    int ret = reminder_delete(id);
    if (ret == 0) {
        uart_puts(tr("Reminder deleted.\n", "提醒已删除。\n"));
        LOG_INFO_T("CmdReminder", "Delete", "OK", "deleted reminder %s", id);
    } else {
        uart_puts(tr("Failed to delete reminder.\n", "删除提醒失败。\n"));
        LOG_ERROR_T("CmdReminder", "Delete", "Fail", "reminder_delete returned %d", ret);
    }
}

static void cmd_reminder_help(void) {
    uart_puts(tr("\nReminder Commands:\n", "\n提醒命令：\n"));
    uart_puts(tr("  remind add \"<content>\" <time> [repeat]  - Add a reminder\n",
                 "  remind add \"<内容>\" <时间> [重复]  - 添加提醒\n"));
    uart_puts(tr("  remind list                           - List pending reminders\n",
                 "  remind list                           - 列出待触发提醒\n"));
    uart_puts(tr("  remind delete <id>                    - Delete a reminder\n",
                 "  remind delete <ID>                    - 删除提醒\n"));
    uart_puts(tr("\nTime formats:\n", "\n时间格式：\n"));
    uart_puts(tr("  YYYY-MM-DD HH:MM:SS  - Absolute time\n",
                 "  YYYY-MM-DD HH:MM:SS  - 绝对时间\n"));
    uart_puts(tr("  +N                   - N minutes from now\n",
                 "  +N                   - N 分钟后\n"));
    uart_puts(tr("  now                  - Immediately\n",
                 "  now                  - 立即\n"));
    uart_puts(tr("  tomorrow             - Tomorrow at same time\n",
                 "  tomorrow             - 明天同一时间\n"));
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void reminder_dispatch(const char *args) {
    LOG_DEBUG_T("CmdReminder", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        cmd_reminder_help();
        return;
    }

    char cmd_buf[512];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *arg1 = strtok_r(NULL, "", &saveptr);

    if (!subcmd) {
        cmd_reminder_help();
        return;
    }

    if (strcmp(subcmd, "add") == 0) {
        if (!arg1 || !*arg1) {
            cmd_reminder_add(NULL, NULL, 0, 0);
            return;
        }

        /* 解析: remind add "content" time [repeat] */
        char *content_start = strchr(arg1, '"');
        if (content_start) {
            content_start++;
            char *content_end = strchr(content_start, '"');
            if (content_end) {
                *content_end = '\0';
                char *remaining = content_end + 1;
                while (*remaining == ' ') remaining++;

                char time_buf[64];
                int repeat = 0;
                int interval = 60;

                if (*remaining) {
                    char *next = strchr(remaining, ' ');
                    if (next) {
                        *next = '\0';
                        safe_strncpy(time_buf, remaining, sizeof(time_buf));
                        repeat = atoi(next + 1);
                        if (repeat < 0) repeat = 0;
                    } else {
                        safe_strncpy(time_buf, remaining, sizeof(time_buf));
                    }
                    cmd_reminder_add(content_start, time_buf, repeat, interval);
                } else {
                    uart_puts(tr("Please specify a time.\n", "请指定时间。\n"));
                }
                return;
            }
        }

        /* 没有引号，尝试直接解析 */
        char *space = strchr(arg1, ' ');
        if (space) {
            *space = '\0';
            char *time_part = space + 1;
            while (*time_part == ' ') time_part++;
            cmd_reminder_add(arg1, time_part, 0, 60);
        } else {
            uart_puts(tr("Usage: remind add \"<content>\" <time>\n", "用法：remind add \"<内容>\" <时间>\n"));
        }
    } else if (strcmp(subcmd, "list") == 0) {
        cmd_reminder_list();
    } else if (strcmp(subcmd, "delete") == 0) {
        if (arg1 && *arg1) {
            /* 去除前导空格 */
            while (*arg1 == ' ') arg1++;
            cmd_reminder_delete(arg1);
        } else {
            uart_puts(tr("Usage: remind delete <id>\n", "用法：remind delete <ID>\n"));
        }
    } else if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0) {
        cmd_reminder_help();
    } else {
        uart_puts(tr("Unknown reminder command: ", "未知提醒命令："));
        uart_puts(subcmd);
        uart_puts("\n");
        uart_puts(tr("Available: add, list, delete, help\n",
                     "可用命令：add, list, delete, help\n"));
    }
}