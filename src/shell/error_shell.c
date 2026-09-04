/**
 * @file    src/shell/error_shell.c
 * @brief   紧急修复终端（修复 repair 自动返回，增加权限修复指导）
 * @version LN-0.4.3
 * @changes 增加 fix-perms 命令；增加权限修复指导信息；
 *          添加 safe_string.h 头文件；
 *          移除 ensure_daemon_running/ensure_ai_server_running 依赖，改用 system 启动服务
 */

#include "error_shell.h"
#include "lang.h"
#include "uart.h"
#include "log_extra.h"
#include "self_check.h"
#include "data_path.h"
#include "safe_string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static void execute_host_command(const char *cmd) {
    if (!cmd || !*cmd) return;
    pid_t pid = fork();
    if (pid == -1) {
        uart_puts(tr("fork failed\n", "fork 失败\n"));
        return;
    }
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        perror("execlp");
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}

void error_shell_run(void) {
    uart_puts(COLOR_RESET);
    uart_puts(tr("\n!!! EMERGENCY ERROR SHELL !!!\n", "\n!!! 紧急修复终端 !!!\n"));
    
    const char *reason = get_last_selfcheck_error();
    if (reason && reason[0] != '\0') {
        uart_puts(tr("Reason: ", "原因: "));
        uart_puts(reason);
        uart_puts("\n");
    } else {
        uart_puts(tr("Reason: Unknown (self-check failed)\n", "原因：未知（自检失败）\n"));
    }

    uart_puts(tr("\nAvailable commands:\n", "可用命令：\n"));
    uart_puts(tr("  repair   - Retry self-check and restart services (auto-return on success)\n", "  repair   - 重试自检并重启服务（成功后自动返回）\n"));
    uart_puts(tr("  fix-perms - Fix common permission issues\n", "  fix-perms - 修复常见权限问题\n"));
    uart_puts(tr("  shell    - Force enter normal shell (risk)\n", "  shell    - 强行进入普通 Shell（风险）\n"));
    uart_puts(tr("  reset    - Delete /LINGOS and reinitialize (type YES)\n", "  reset    - 删除 /LINGOS 并重新初始化（输入 YES）\n"));
    uart_puts(tr("  exit     - Exit program\n", "  exit     - 退出程序\n"));
    uart_puts(tr("Additionally, you can directly execute any host command.\n",
                 "此外，您可以直接执行任意宿主命令。\n"));
    uart_puts(tr("Manual start commands:\n", "手动启动命令：\n"));
    uart_puts(tr("  python3 /LINGOS/bin/ai_server.py &\n", "  python3 /LINGOS/bin/ai_server.py &\n"));
    uart_puts(tr("  ./lingosd &\n", "  ./lingosd &\n"));
    uart_puts(tr("  tail -f /LINGOS/Debug/ai_server.log\n", "  tail -f /LINGOS/Debug/ai_server.log\n"));
    uart_puts(tr("  chmod -R 755 /LINGOS/  # Fix permissions\n", "  chmod -R 755 /LINGOS/  # 修复权限\n"));

    char cmd[512];
    while (1) {
        uart_puts(COLOR_RESET);
        uart_puts("error> ");
        int idx = 0;
        while (1) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                cmd[idx] = '\0';
                break;
            }
            if (c == '\b' || c == 127) {
                if (idx > 0) {
                    idx--;
                    uart_puts("\b \b");
                }
                continue;
            }
            if (idx < (int)sizeof(cmd) - 1) {
                cmd[idx++] = c;
                uart_putc(c);
            }
        }
        if (idx == 0) continue;

        if (strcmp(cmd, "repair") == 0) {
            uart_puts(tr("Running self-check...\n", "正在运行自检...\n"));
            if (self_check_and_sync() == 0) {
                uart_puts(tr("Self-check passed. Starting services...\n", "自检通过，正在启动服务...\n"));
                /* 启动 lingosd */
                const char *root = lingos_data_root();
                char cmd_buf[512];
                safe_snprintf(cmd_buf, sizeof(cmd_buf), "cd %s && ./lingosd > /dev/null 2>&1 &", root);
                system(cmd_buf);
                uart_puts(tr("Started lingosd.\n", "已启动 lingosd。\n"));
                /* 启动 ai_server.py */
                safe_snprintf(cmd_buf, sizeof(cmd_buf), "python3 %s/bin/ai_server.py > /dev/null 2>&1 &", root);
                system(cmd_buf);
                uart_puts(tr("Started AI server.\n", "已启动 AI 服务器。\n"));
                uart_puts(tr("Services started. Returning to normal startup.\n", "服务已启动，返回正常启动。\n"));
                return;
            } else {
                uart_puts(tr("Self-check still failed.\n", "自检仍然失败。\n"));
                uart_puts(tr("  Try 'fix-perms' to fix permission issues, or 'reset' to reset.\n",
                             "  尝试 'fix-perms' 修复权限问题，或 'reset' 重置。\n"));
            }
        } else if (strcmp(cmd, "fix-perms") == 0) {
            uart_puts(tr("Fixing common permission issues...\n", "正在修复常见权限问题...\n"));
            const char *root = lingos_data_root();
            char cmd_buf[512];
            safe_snprintf(cmd_buf, sizeof(cmd_buf), "chmod -R 755 '%s' 2>/dev/null", root);
            system(cmd_buf);
            safe_snprintf(cmd_buf, sizeof(cmd_buf), "chown -R $(whoami) '%s' 2>/dev/null", root);
            system(cmd_buf);
            uart_puts(tr("Permission fix completed. Try 'repair' again.\n", "权限修复完成。请再次尝试 'repair'。\n"));
        } else if (strcmp(cmd, "shell") == 0) {
            uart_puts(tr("Forcing normal shell. Use with caution.\n", "强行进入普通 Shell。请谨慎操作。\n"));
            return;
        } else if (strcmp(cmd, "reset") == 0) {
            uart_puts(tr("Type 'YES' to confirm reset: ", "输入 'YES' 确认重置: "));
            char confirm[16];
            int i = 0;
            while (1) {
                char c = uart_getc();
                if (c == '\r' || c == '\n') break;
                if (i < 15) confirm[i++] = c;
            }
            confirm[i] = '\0';
            uart_puts("\n");
            if (strcmp(confirm, "YES") == 0) {
                const char *root = lingos_data_root();
                char cmd_buf[512];
                snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf '%s'", root);
                system(cmd_buf);
                uart_puts(tr("Reset done. Please restart LING OS.\n", "重置完成。请重启 LING OS。\n"));
                return;
            } else {
                uart_puts(tr("Reset cancelled.\n", "重置已取消。\n"));
            }
        } else if (strcmp(cmd, "exit") == 0) {
            uart_puts(tr("Exiting.\n", "退出。\n"));
            exit(0);
        } else {
            execute_host_command(cmd);
        }
    }
}