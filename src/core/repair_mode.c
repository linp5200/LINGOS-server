/**
 * @file    src/core/repair_mode.c
 * @brief   修复模式实现（异常关闭后的恢复选项）
 * @version LN-0.4.3
 * @par     核心协议：防弹编程
 */

#include "repair_mode.h"
#include "exit_status.h"
#include "backup.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../shell/error_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ============================================================
 * 辅助：显示修复菜单
 * ============================================================ */
/* ============================================================
 * 【2026-10-02 修复】框线工具：UTF-8 感知宽度 + 逐行加框
 *   · R3 修复：多行消息（exit_status 报告）逐行加框——原实现直接输出破坏框线
 *   · 对齐修复：中英混排行宽统一（原 60/61/62 不一致）
 * ============================================================ */
#define REPAIR_BOX_INNER 60

static int repair_disp_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return 0;
    while (*p) {
        if (p[0] < 0x80) { w += 1; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0) { w += 1; p += 2; }
        else if ((p[0] & 0xF0) == 0xE0) {
            unsigned int cp = (unsigned int)((p[0] & 0x0F) << 12)
                            | (unsigned int)((p[1] & 0x3F) << 6)
                            | (unsigned int)(p[2] & 0x3F);
            w += (cp >= 0x2E80) ? 2 : 1;   /* CJK 区段按 2 宽；符号（⚠ 等）按 1 */
            p += 3;
        }
        else if ((p[0] & 0xF8) == 0xF0) { w += 2; p += 4; }
        else { p += 1; }
    }
    return w;
}

/* 输出一行框内容：║ + 内容（2 空格边距）+ 右侧填充 + ║ */
static void repair_box_line(const char *text) {
    char content[512];
    safe_snprintf(content, sizeof(content), "  %s", text ? text : "");

    /* 截断到内宽-1（防溢出框线；按 UTF-8 字符边界安全截断） */
    while (repair_disp_width(content) > REPAIR_BOX_INNER - 1 && content[0]) {
        size_t l = strlen(content);
        if (l == 0) break;
        size_t cut = l - 1;
        while (cut > 0 && ((unsigned char)content[cut] & 0xC0) == 0x80) cut--;
        content[cut] = '\0';
    }

    int pad = REPAIR_BOX_INNER - repair_disp_width(content);
    if (pad < 0) pad = 0;
    char out[680];
    safe_snprintf(out, sizeof(out), "║%s%*s║\n", content, pad, "");
    uart_puts(out);
}

/* 多行文本逐行加框（R3 修复核心） */
static void repair_box_multiline(const char *text) {
    char buf[512];
    safe_strncpy(buf, text ? text : "", sizeof(buf));
    if (!buf[0]) { repair_box_line(""); return; }
    char *p = buf;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t l = strlen(p);
        if (l && p[l-1] == '\r') p[l-1] = '\0';
        repair_box_line(p);
        if (!nl) break;
        if (!*(nl + 1)) break;   /* 结尾换行——不再多打空行 */
        p = nl + 1;
    }
}

static void display_repair_menu(const exit_status_t *status) {
    char msg[512];
    exit_status_format_message(status, "en", msg, sizeof(msg));

    uart_puts(COLOR_YELLOW);
    uart_puts("\n╔════════════════════════════════════════════════════════════╗\n");
    repair_box_line("");
    repair_box_line(tr("⚠  LING OS was not shut down properly",
                       "⚠  LING OS 上次未正常关闭"));
    repair_box_line("");
    repair_box_line(tr("The system may have crashed or been powered off.",
                       "系统可能已崩溃或断电。"));
    repair_box_line("");
    repair_box_multiline(msg);   /* 多行报告——逐行加框（R3 修复） */
    repair_box_line("");
    repair_box_line(tr("Options:", "选项："));
    repair_box_line(tr("[1] Auto Repair (recommended)", "[1] 自动修复（推荐）"));
    repair_box_line(tr("[2] Restore from backup", "[2] 从备份恢复"));
    repair_box_line(tr("[3] Manual Repair (emergency shell)", "[3] 手动修复（紧急终端）"));
    repair_box_line(tr("[4] Ignore and continue", "[4] 忽略并继续"));
    repair_box_line("");
    repair_box_line(tr("Enter choice (1-4):", "输入选项 (1-4)："));
    uart_puts(COLOR_YELLOW);
    uart_puts("╚════════════════════════════════════════════════════════════╝\n");
    uart_puts(COLOR_RESET);
    uart_puts("\n");
}

/* ============================================================
 * 辅助：读取用户选择（无超时）
 * ============================================================ */
static int read_user_choice(void) {
    char input[16];
    int idx = 0;

    while (1) {
        int c = getchar();
        if (c == EOF) return -1;
        if (c == '\n' || c == '\r') {
            input[idx] = '\0';
            break;
        }
        if (idx < (int)sizeof(input) - 1) {
            input[idx++] = (char)c;
        }
    }

    if (input[0] == '\0') return -1;
    int choice = atoi(input);
    if (choice >= 1 && choice <= 4) return choice;
    return -1;
}

/* ============================================================
 * 修复选项执行
 * ============================================================ */
static int do_auto_repair(void) {
    LOG_INFO_T("RepairMode", "AutoRepair", "Enter", "starting auto repair");

    uart_puts(tr("\n🔧 Running auto repair...\n", "\n🔧 正在执行自动修复...\n"));

    /* 1. 运行自检 */
    extern int self_check_and_sync(void);
    extern int need_configuration;

    int ret = self_check_and_sync();

    if (ret == 0 && !need_configuration) {
        uart_puts(tr("✅ Auto repair completed successfully.\n",
                     "✅ 自动修复成功完成。\n"));
        LOG_INFO_T("RepairMode", "AutoRepair", "OK", "auto repair successful");
        return 0;
    } else if (need_configuration) {
        uart_puts(tr("⚠ Auto repair: Configuration is incomplete.\n",
                     "⚠ 自动修复：配置不完整。\n"));
        uart_puts(tr("   Please run 'system configuration' manually.\n",
                     "   请手动运行 'system configuration'。\n"));
        LOG_WARN_T("RepairMode", "AutoRepair", "ConfigMissing", "configuration incomplete");
        return -1;
    } else {
        uart_puts(tr("❌ Auto repair failed. Please try manual repair.\n",
                     "❌ 自动修复失败。请尝试手动修复。\n"));
        LOG_ERROR_T("RepairMode", "AutoRepair", "Fail", "auto repair failed");
        return -1;
    }
}

static int do_restore_backup(void) {
    LOG_INFO_T("RepairMode", "RestoreBackup", "Enter", "restoring from backup");

    extern int backup_restore_latest(void);
    extern void backup_list(void);

    uart_puts(tr("\n📦 Available backups:\n", "\n📦 可用备份：\n"));
    backup_list();

    uart_puts(tr("\nRestoring latest backup...\n", "\n正在恢复最新备份...\n"));

    int ret = backup_restore_latest();
    if (ret == 0) {
        uart_puts(tr("✅ Backup restored successfully.\n", "✅ 备份恢复成功。\n"));
        uart_puts(tr("   System will restart.\n", "   系统将重启。\n"));
        LOG_INFO_T("RepairMode", "RestoreBackup", "OK", "backup restored");
        /* 通知用户重启 */
        exit_status_clear_abnormal();
        return 0;
    } else {
        uart_puts(tr("❌ Backup restore failed. No valid backup found.\n",
                     "❌ 备份恢复失败。未找到有效的备份。\n"));
        LOG_ERROR_T("RepairMode", "RestoreBackup", "Fail", "backup restore failed");
        return -1;
    }
}

static int do_manual_repair(void) {
    LOG_INFO_T("RepairMode", "ManualRepair", "Enter", "entering manual repair shell");

    uart_puts(tr("\n🔧 Entering manual repair shell...\n", "\n🔧 进入手动修复终端...\n"));
    uart_puts(tr("   Type 'exit' to return to repair menu.\n",
                 "   输入 'exit' 返回修复菜单。\n"));

    error_shell_run();

    LOG_INFO_T("RepairMode", "ManualRepair", "Exit", "exited manual repair shell");
    return 0;
}

static int do_ignore_continue(void) {
    LOG_INFO_T("RepairMode", "IgnoreContinue", "Enter", "ignoring abnormal status");

    exit_status_clear_abnormal();

    uart_puts(tr("✅ Ignored, continuing normal startup.\n",
                 "✅ 已忽略，继续正常启动。\n"));
    LOG_INFO_T("RepairMode", "IgnoreContinue", "OK", "ignoring abnormal status, continuing");

    return 0;
}

/* ============================================================
 * 核心 API 实现
 * ============================================================ */
int repair_mode_run(void) {
    LOG_INFO_T("RepairMode", "Run", "Enter", "starting repair mode");

    const exit_status_t *status = exit_status_get();

    /* 检查是否有异常退出标记 */
    if (status->is_clean_exit) {
        LOG_DEBUG_T("RepairMode", "Run", "CleanExit", "no abnormal exit, skipping");
        return 0;
    }

    /* 【2026-09-19 修复】非交互环境（nohup/后台/无 tty）不进入交互菜单——
     *   原实现 getchar() 立即 EOF → 菜单无限循环（supervisor 自动重启场景必触发）。
     *   改为：跑一次自动修复（尽力而为）→ 清除标记 → 继续启动。 */
    if (!isatty(STDIN_FILENO)) {
        LOG_WARN_T("RepairMode", "Run", "NonInteractive",
                   "no tty — running non-interactive auto repair, then continuing");
        int r = do_auto_repair();
        exit_status_clear_abnormal();
        LOG_INFO_T("RepairMode", "Run", "NonInteractiveDone",
                   "auto repair result=%d, continuing startup", r);
        return 0;
    }

    /* 检查连续崩溃次数，如果超过阈值且时间窗口内，可能自动执行自动修复 */
    if (status->crash_count >= 3 &&
        (time(NULL) - status->first_crash_time) < 300) {
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("\n⚠ System has crashed multiple times in a short period.\n",
                     "\n⚠ 系统在短时间内多次崩溃。\n"));
        uart_puts(tr("   Running auto repair automatically...\n",
                     "   正在自动执行自动修复...\n"));
        uart_puts(COLOR_RESET);
        return repair_mode_auto();
    }

    while (1) {
        display_repair_menu(status);

        uart_puts(tr("Enter choice (1-4): ", "输入选项 (1-4): "));

        int choice = read_user_choice();

        if (choice < 1 || choice > 4) {
            uart_puts(tr("Invalid choice. Please enter 1-4.\n",
                         "无效选项。请输入 1-4。\n"));
            continue;
        }

        switch (choice) {
            case 1:
                if (do_auto_repair() == 0) {
                    exit_status_clear_abnormal();
                    return 0;
                }
                break;
            case 2:
                if (do_restore_backup() == 0) {
                    /* 备份恢复后需要重启，但这里返回 0 让主程序继续 */
                    exit_status_clear_abnormal();
                    return 0;
                }
                break;
            case 3:
                do_manual_repair();
                /* 手动修复后重新显示菜单 */
                break;
            case 4:
                do_ignore_continue();
                return 0;
            default:
                break;
        }

        uart_puts(tr("\nPress Enter to continue...", "\n按 Enter 继续..."));
        getchar();
        uart_puts("\n");
    }

    return -1;
}

int repair_mode_auto(void) {
    LOG_INFO_T("RepairMode", "Auto", "Enter", "auto repair mode (non-interactive)");

    /* 尝试自动修复 */
    int ret = do_auto_repair();

    if (ret == 0) {
        exit_status_clear_abnormal();
        LOG_INFO_T("RepairMode", "Auto", "OK", "auto repair successful");
        return 0;
    }

    /* 如果自动修复失败，尝试恢复备份 */
    uart_puts(tr("\nAuto repair failed, trying backup restore...\n",
                 "\n自动修复失败，正在尝试恢复备份...\n"));
    ret = do_restore_backup();

    if (ret == 0) {
        exit_status_clear_abnormal();
        LOG_INFO_T("RepairMode", "Auto", "OK", "backup restore successful");
        return 0;
    }

    /* 如果备份恢复也失败，进入手动修复 */
    uart_puts(tr("\nBackup restore failed, entering manual repair shell...\n",
                 "\n备份恢复失败，正在进入手动修复终端...\n"));
    do_manual_repair();

    LOG_WARN_T("RepairMode", "Auto", "Partial", "auto repair completed with manual intervention");
    return -1;
}