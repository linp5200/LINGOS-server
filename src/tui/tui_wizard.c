/**
 * @file    src/tui/tui_wizard.c
 * @brief   TUI 配置向导渲染器（集成 wizard_core 框架，三层降级）
 * @version LN-0.4.3
 * @changes 增强 is_tui_available() 诊断日志；
 *          新增 tui_ask_user() 交互函数，在检测失败时询问用户；
 *          修改 tui_wizard_run() 流程，支持用户强制使用 TUI；
 *          若强制使用仍失败，提示缺少 libnotcurses 并建议手动安装。
 */

#include "tui_wizard.h"
#include "tui_renderer.h"
#include "tui_defensive.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../wizard/wizard_core.h"
#include "../wizard/cli_wizard.h"
#include "../wizard/raw_wizard.h"
#include "../drivers/uart.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>

static int g_tui_failures = 0;
static sigjmp_buf g_jmp_buffer;

static void tui_sigsegv_handler(int sig) {
    (void)sig;
    LOG_WARN_T("TUIWizard", "Signal", "Segfault", "caught SIGSEGV, jumping to recovery");
    siglongjmp(g_jmp_buffer, 1);
}

/**
 * @brief 检测 TUI 是否可用（增强日志）
 */
static int is_tui_available(void) {
    LOG_DEBUG_T("TUIWizard", "IsAvailable", "Enter", "checking TUI availability");

    /* 检查是否在终端中运行 */
    int is_stdin_tty = isatty(STDIN_FILENO);
    int is_stdout_tty = isatty(STDOUT_FILENO);
    LOG_DEBUG_T("TUIWizard", "IsAvailable", "TTY", "stdin_tty=%d, stdout_tty=%d", is_stdin_tty, is_stdout_tty);

    if (!is_stdin_tty || !is_stdout_tty) {
        LOG_DEBUG_T("TUIWizard", "IsAvailable", "NoTTY", "not a terminal");
        return 0;
    }

    const char *term = getenv("TERM");
    LOG_DEBUG_T("TUIWizard", "IsAvailable", "Term", "TERM=%s", term ? term : "(null)");

    if (!term || strstr(term, "dumb")) {
        LOG_DEBUG_T("TUIWizard", "IsAvailable", "BadTerm", "TERM=%s, not suitable for TUI", term ? term : "(null)");
        return 0;
    }

    /* 尝试初始化 Notcurses 检查库是否可用 */
    struct notcurses_options opts = {
        .flags = NCOPTION_INHIBIT_SETLOCALE,
        .loglevel = NCLOGLEVEL_FATAL,
    };
    struct notcurses *nc = notcurses_init(&opts, NULL);

    if (!nc) {
        LOG_WARN_T("TUIWizard", "IsAvailable", "NotcursesInitFail", "notcurses_init failed, TUI unavailable");
        return 0;
    }

    notcurses_stop(nc);
    LOG_DEBUG_T("TUIWizard", "IsAvailable", "OK", "TUI available");
    return 1;
}

/**
 * @brief 询问用户是否强制使用 TUI（新增交互）
 */
static int tui_ask_user(void) {
    LOG_INFO_T("TUIWizard", "AskUser", "Enter", "asking user about TUI support");

    const char *term = getenv("TERM");
    char terminal_info[256];
    safe_snprintf(terminal_info, sizeof(terminal_info),
                  "TERM=%s, isatty(stdin)=%d, isatty(stdout)=%d",
                  term ? term : "(null)", isatty(STDIN_FILENO), isatty(STDOUT_FILENO));

    uart_puts(COLOR_YELLOW);
    uart_puts(tr("\n⚠ Current terminal may not support TUI.\n",
                 "\n⚠ 当前终端可能不支持 TUI。\n"));
    uart_puts(COLOR_DIM);
    uart_puts(tr("Terminal environment details:\n", "终端环境详细信息：\n"));
    uart_puts("  ");
    uart_puts(terminal_info);
    uart_puts("\n");
    uart_puts(COLOR_RESET);

    uart_puts(tr("\nPlease choose:\n", "\n请选择：\n"));
    uart_puts(tr("  [1] My terminal supports TUI (force try)\n",
                 "  [1] 我的终端支持 TUI（强制尝试）\n"));
    uart_puts(tr("  [2] My terminal does NOT support TUI (fallback to CLI)\n",
                 "  [2] 我的终端不支持 TUI（降级到 CLI）\n"));
    uart_puts(tr("Enter choice (1/2): ", "输入选择 (1/2): "));

    char c = uart_getc();
    uart_putc(c);
    uart_puts("\n");

    if (c == '1') {
        LOG_INFO_T("TUIWizard", "AskUser", "Force", "user chose to force TUI");
        return 1;
    } else {
        LOG_INFO_T("TUIWizard", "AskUser", "Fallback", "user chose to fallback to CLI");
        return 0;
    }
}

/**
 * @brief 显示 libnotcurses 安装提示
 */
static void tui_show_install_help(void) {
    uart_puts(COLOR_RED);
    uart_puts(tr("\n❌ TUI initialization failed even after user forced it.\n",
                 "\n❌ 即使强制尝试，TUI 初始化仍然失败。\n"));
    uart_puts(COLOR_YELLOW);
    uart_puts(tr("This usually means libnotcurses is not installed or not properly linked.\n",
                 "这通常意味着 libnotcurses 未安装或未正确链接。\n"));
    uart_puts(tr("\nPlease install libnotcurses manually:\n",
                 "\n请手动安装 libnotcurses：\n"));
    uart_puts(tr("  Debian/Ubuntu: apt install libnotcurses-dev\n",
                 "  Debian/Ubuntu: apt install libnotcurses-dev\n"));
    uart_puts(tr("  Fedora:       dnf install notcurses-devel\n",
                 "  Fedora:       dnf install notcurses-devel\n"));
    uart_puts(tr("  Arch:         pacman -S notcurses\n",
                 "  Arch:         pacman -S notcurses\n"));
    uart_puts(tr("\nSystem will continue in CLI mode. Some TUI features will be unavailable.\n",
                 "\n系统将以 CLI 模式继续运行，部分 TUI 功能将不可用。\n"));
    uart_puts(COLOR_RESET);
}

/**
 * @brief 运行 TUI 配置向导（增强交互）
 */
int tui_wizard_run(wizard_state_t *state) {
    LOG_INFO_T("TUIWizard", "Run", "Enter", "state=%p", (void*)state);
    if (!state) return -1;

    if (!is_tui_available()) {
        LOG_WARN_T("TUIWizard", "Run", "TUIUnavailable", "TUI not available, asking user");
        int force = tui_ask_user();
        if (!force) {
            LOG_INFO_T("TUIWizard", "Run", "UserFallback", "user chose CLI fallback");
            return cli_wizard_run(state);
        }
        /* 用户强制使用，继续尝试初始化 */
        LOG_INFO_T("TUIWizard", "Run", "UserForce", "user forced TUI, attempting init");
    }

    /* 安装信号处理器用于崩溃恢复 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tui_sigsegv_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    if (sigsetjmp(g_jmp_buffer, 1) != 0) {
        g_tui_failures++;
        LOG_WARN_T("TUIWizard", "Run", "CrashRecover", "TUI crashed, failures=%d", g_tui_failures);
        if (g_tui_failures >= 3) {
            LOG_ERROR_T("TUIWizard", "Run", "TooManyFailures", "TUI failed 3 times, falling back to CLI");
            sa.sa_handler = SIG_DFL;
            sigaction(SIGSEGV, &sa, NULL);
            sigaction(SIGABRT, &sa, NULL);
            log_set_console_output(1);
            tui_show_install_help();
            return cli_wizard_run(state);
        }
        log_set_console_output(1);
        return cli_wizard_run(state);
    }

    /* 挂起日志控制台输出 */
    log_set_console_output(0);

    /* 初始化 Notcurses */
    struct notcurses_options opts = {
        .flags = NCOPTION_INHIBIT_SETLOCALE,
        .loglevel = NCLOGLEVEL_FATAL,
    };
    struct notcurses *nc = notcurses_init(&opts, NULL);
    if (!nc) {
        LOG_ERROR_T("TUIWizard", "Run", "NotcursesInitFail", "notcurses_init failed");
        log_set_console_output(1);
        g_tui_failures++;
        tui_show_install_help();
        return cli_wizard_run(state);
    }

    struct ncplane *stdplane = notcurses_stdplane(nc);
    if (!stdplane) {
        LOG_ERROR_T("TUIWizard", "Run", "NoStdplane", "notcurses_stdplane failed");
        notcurses_stop(nc);
        log_set_console_output(1);
        g_tui_failures++;
        tui_show_install_help();
        return cli_wizard_run(state);
    }

    int ret = 0;
    while (!state->cancelled && state->current_index < state->step_count) {
        wizard_step_t *step = wizard_core_get_current_step(state);
        if (!step) {
            LOG_WARN_T("TUIWizard", "Run", "NoStep", "no step at index %d", state->current_index);
            break;
        }

        LOG_DEBUG_T("TUIWizard", "Run", "Step", "processing step '%s' (index=%d)", step->id, state->current_index);

        if (step->on_enter) {
            if (step->on_enter(state->ctx) != 0) {
                LOG_ERROR_T("TUIWizard", "Run", "OnEnterFail", "step '%s' on_enter failed", step->id);
                ret = -1;
                break;
            }
        }
        step->state = STEP_STATE_ACTIVE;

        if (step->on_render) {
            int render_ret = step->on_render(state->ctx, nc);
            if (render_ret != 0) {
                g_tui_failures++;
                LOG_WARN_T("TUIWizard", "Run", "RenderFail", "step '%s' render failed (ret=%d), failures=%d",
                           step->id, render_ret, g_tui_failures);
                if (g_tui_failures >= 3) {
                    LOG_ERROR_T("TUIWizard", "Run", "TooManyFailures", "falling back to CLI");
                    ret = cli_wizard_run(state);
                    break;
                }
            }
        }

        notcurses_render(nc);

        ncinput input;
        int key = notcurses_get_blocking(nc, &input);

        if (key == 'q' || key == 'Q') {
            wizard_core_set_cancelled(state);
            LOG_INFO_T("TUIWizard", "Run", "Quit", "user quit");
            ret = -1;
            break;
        }

        if (step->on_key) {
            int result = step->on_key(state->ctx, key);
            if (result == 1) {
                step->state = STEP_STATE_COMPLETED;
                state->completed_count++;
                int next = wizard_core_next_step(state);
                if (next == 1) {
                    LOG_DEBUG_T("TUIWizard", "Run", "AllStepsDone", "all steps completed");
                    break;
                }
            } else if (result == -1) {
                wizard_core_set_cancelled(state);
                LOG_WARN_T("TUIWizard", "Run", "StepCancel", "step '%s' cancelled", step->id);
                ret = -1;
                break;
            }
        } else {
            /* 如果没有 on_key，自动推进 */
            step->state = STEP_STATE_COMPLETED;
            state->completed_count++;
            if (wizard_core_next_step(state) == 1) break;
        }
    }

    notcurses_stop(nc);
    log_set_console_output(1);

    /* 恢复信号默认处理 */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* 如果所有步骤完成且未取消，合并配置 */
    if (!state->cancelled && state->current_index >= state->step_count) {
        LOG_INFO_T("TUIWizard", "Run", "Complete", "all %d steps completed", state->step_count);
        if (wizard_core_merge_config(state->ctx) != 0) {
            LOG_ERROR_T("TUIWizard", "Run", "MergeFail", "config merge failed");
            ret = -1;
        } else {
            ret = 0;
        }
    } else if (state->cancelled) {
        LOG_INFO_T("TUIWizard", "Run", "Cancelled", "wizard cancelled");
        ret = -1;
    }

    LOG_INFO_T("TUIWizard", "Run", "Exit", "ret=%d", ret);
    return ret;
}