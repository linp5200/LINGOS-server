/**
 * @file    crash_handler.c
 * @brief   崩溃处理信号捕获和日志转储（含系统转储收集）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include "crash_handler.h"
#include "crash_dump.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/* 【musl兼容】execinfo/backtrace 仅 glibc 提供——musl（Alpine）跳过崩溃回溯 */
#if defined(__GLIBC__)
#include <execinfo.h>
#define HAS_BACKTRACE 1
#else
#define HAS_BACKTRACE 0
#endif

#define MAX_BACKTRACE_FRAMES 100
#define LOG_DUMP_PATH "/Debug/log/crash_"

static int crash_log_fd = -1;

/* ============================================================
 * 异步安全字符串写入
 * ============================================================ */

static void safe_write(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t ret = write(fd, buf, len);
        if (ret <= 0) break;
        buf += ret;
        len -= ret;
    }
}

static void safe_write_string(int fd, const char *str) {
    if (str) safe_write(fd, str, strlen(str));
}

static void get_timestamp_safe(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, size, "%Y%m%d_%H%M%S", &tm);
}

/* ============================================================
 * 写入崩溃日志（异步安全）
 * ============================================================ */

static void write_crash_log_safe(const char *reason, const char *signal_name, void *ucontext) {
    if (crash_log_fd < 0) return;

    safe_write_string(crash_log_fd, "=== LING OS Crash Report ===\n");
    safe_write_string(crash_log_fd, "Reason: ");
    safe_write_string(crash_log_fd, reason ? reason : "unknown");
    safe_write_string(crash_log_fd, "\nSignal: ");
    safe_write_string(crash_log_fd, signal_name ? signal_name : "unknown");
    safe_write_string(crash_log_fd, "\nPID: ");
    char pidbuf[16];
    safe_snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
    safe_write_string(crash_log_fd, pidbuf);

    char timestamp[32];
    get_timestamp_safe(timestamp, sizeof(timestamp));
    safe_write_string(crash_log_fd, "Timestamp: ");
    safe_write_string(crash_log_fd, timestamp);
    safe_write_string(crash_log_fd, "\n");

    void *buffer[MAX_BACKTRACE_FRAMES];
#if HAS_BACKTRACE
    int frames = backtrace(buffer, MAX_BACKTRACE_FRAMES);
    char **symbols = backtrace_symbols(buffer, frames);
    safe_write_string(crash_log_fd, "\n=== Backtrace ===\n");
    for (int i = 0; i < frames && i < 50; i++) {
        safe_write_string(crash_log_fd, "  ");
        if (symbols && symbols[i]) safe_write_string(crash_log_fd, symbols[i]);
        safe_write_string(crash_log_fd, "\n");
    }
    if (symbols) free(symbols);
#else
    (void)buffer;
    safe_write_string(crash_log_fd, "\n=== Backtrace ===\n  (not available on musl)\n");
#endif

#ifdef __aarch64__
    if (ucontext) {
        ucontext_t *uc = (ucontext_t *)ucontext;
        safe_write_string(crash_log_fd, "\n=== Registers (ARM64) ===\n");
        char regbuf[256];
        safe_snprintf(regbuf, sizeof(regbuf), "PC: 0x%lx\n", (unsigned long)uc->uc_mcontext.pc);
        safe_write_string(crash_log_fd, regbuf);
        safe_snprintf(regbuf, sizeof(regbuf), "SP: 0x%lx\n", (unsigned long)uc->uc_mcontext.sp);
        safe_write_string(crash_log_fd, regbuf);
        safe_snprintf(regbuf, sizeof(regbuf), "LR: 0x%lx\n", (unsigned long)uc->uc_mcontext.regs[30]);
        safe_write_string(crash_log_fd, regbuf);
    }
#endif

    close(crash_log_fd);
    crash_log_fd = -1;
}

/* ============================================================
 * 信号处理函数（异步安全）
 * ============================================================ */

static void crash_signal_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)info;
    (void)ucontext;

    const char *sig_name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sig_name = "SIGSEGV (Segmentation Fault)"; break;
        case SIGABRT: sig_name = "SIGABRT (Abort)"; break;
        case SIGFPE:  sig_name = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sig_name = "SIGILL (Illegal Instruction)"; break;
        case SIGBUS:  sig_name = "SIGBUS (Bus Error)"; break;
        case SIGTRAP: sig_name = "SIGTRAP (Trace Trap)"; break;
    }

    char reason[128];
    safe_snprintf(reason, sizeof(reason), "Process received signal %d (%s)", sig, sig_name);

    pid_t pid = fork();
    if (pid == 0) {
        write_crash_log_safe(reason, sig_name, ucontext);
        collect_system_dump(sig_name, reason);
        _exit(0);
    } else if (pid > 0) {
        int status;
        int wait_count = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && wait_count < 50) {
            usleep(100000);
            wait_count++;
        }
        if (wait_count >= 50) {
            kill(pid, SIGKILL);
        }
        signal(sig, SIG_DFL);
        raise(sig);
    } else {
        _exit(1);
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void crash_handler_init(void) {
    const char *root = lingos_data_root();
    char log_dir[512];
    safe_snprintf(log_dir, sizeof(log_dir), "%s/Debug/log", root);

    mkdir(log_dir, 0755);

    char timestamp[32];
    get_timestamp_safe(timestamp, sizeof(timestamp));
    char log_path[1024];
    safe_snprintf(log_path, sizeof(log_path), "%s/crash_%s_%d.log", log_dir, timestamp, getpid());

    crash_log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

void crash_dump_log(const char *reason) {
    if (crash_log_fd >= 0) {
        safe_write_string(crash_log_fd, "Manual dump: ");
        safe_write_string(crash_log_fd, reason ? reason : "unknown");
        safe_write_string(crash_log_fd, "\n");
        close(crash_log_fd);
        crash_log_fd = -1;
    }
    if (reason) {
        collect_system_dump("MANUAL", reason);
    } else {
        collect_system_dump("MANUAL", "Manual crash dump");
    }
}