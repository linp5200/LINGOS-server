/**
 * @file    tui_signal.c
 * @brief   TUI 信号处理（捕获致命信号，执行清理）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程
 */

#include "tui_signal.h"
#include "tui_resource.h"
#include "tui_logctl.h"
#include "../lib/log_extra.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 信号处理函数
 * ============================================================ */

static void tui_signal_handler(int sig) {
    const char *sig_name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sig_name = "SIGSEGV"; break;
        case SIGABRT: sig_name = "SIGABRT"; break;
        case SIGFPE:  sig_name = "SIGFPE";  break;
        case SIGBUS:  sig_name = "SIGBUS";  break;
        case SIGTERM: sig_name = "SIGTERM"; break;
        case SIGINT:  sig_name = "SIGINT";  break;
        default:      sig_name = "SIG????"; break;
    }

    LOG_ERROR_T("TuiSignal", "Handler", "Fatal", "caught %s, cleaning up", sig_name);

    /* 恢复日志（如果被挂起） */
    tui_logctl_restore();

    /* 清理所有资源 */
    tui_resource_cleanup_all();

    /* 恢复终端的原始模式（如果 Notcurses 未清理） */
    /* 注意：这里无法调用 notcurses_stop，因为上下文可能已损坏 */
    /* 我们仅恢复信号默认行为并重新触发 */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ============================================================
 * 安装信号处理器
 * ============================================================ */

void tui_signal_setup(void) {
    LOG_DEBUG_T("TuiSignal", "Setup", "Enter", "installing signal handlers");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tui_signal_handler;
    sa.sa_flags = SA_RESETHAND;  /* 执行一次后恢复默认 */
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    LOG_DEBUG_T("TuiSignal", "Setup", "OK", "signal handlers installed");
}