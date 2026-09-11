/**
 * @file    commands.c
 * @brief   调试命令处理（run hello, run time_test 等）
 * @version 2.0.4.0
 *          【0.4.4 修复】time_test 原为 "not yet implemented" 占位 → 补全真实实现
 */

#include "../lib/platform.h"
#include "commands.h"
#include "../common/string_no_sys.h"
#include "../common/safe_string.h"
#include "uart.h"
#include "log_extra.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

static void run_hello(void) {
    uart_puts("Hello from LING OS debug command!\n");
    LOG_INFO_T("DebugCmd", "Hello", "Exec", "hello command executed");
}

/* 【0.4.4】时钟自检：验证 CLOCK_MONOTONIC / CLOCK_REALTIME / gettimeofday 可用且单调 */
static void run_time_test(void) {
    char buf[160];
    struct timespec mono_a, mono_b, real;
    struct timeval tv;

    if (clock_gettime(CLOCK_MONOTONIC, &mono_a) != 0) {
        uart_puts("CLOCK_MONOTONIC: FAILED\n");
        LOG_ERROR_T("DebugCmd", "TimeTest", "MonotonicFail", "clock_gettime failed");
        return;
    }
    if (clock_gettime(CLOCK_REALTIME, &real) != 0) {
        uart_puts("CLOCK_REALTIME: FAILED\n");
        return;
    }
    if (gettimeofday(&tv, NULL) != 0) {
        uart_puts("gettimeofday: FAILED\n");
        return;
    }

    /* 忙等约 10ms，验证单调性 */
    for (volatile int i = 0; i < 200000; i++) { /* spin */ }
    clock_gettime(CLOCK_MONOTONIC, &mono_b);

    long d_ns = (mono_b.tv_sec - mono_a.tv_sec) * 1000000000L
              + (mono_b.tv_nsec - mono_a.tv_nsec);

    uart_puts("Time test:\n");
    safe_snprintf(buf, sizeof(buf), "  CLOCK_MONOTONIC : %ld.%09ld s\n",
                  (long)mono_a.tv_sec, mono_a.tv_nsec);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "  CLOCK_REALTIME  : %ld.%09ld s\n",
                  (long)real.tv_sec, real.tv_nsec);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "  gettimeofday    : %ld.%06ld s\n",
                  (long)tv.tv_sec, (long)tv.tv_usec);
    uart_puts(buf);
    safe_snprintf(buf, sizeof(buf), "  elapsed(mono)   : %ld ns  (%s)\n",
                  d_ns, d_ns > 0 ? "monotonic OK" : "NOT monotonic!");
    uart_puts(buf);
    LOG_INFO_T("DebugCmd", "TimeTest", "Done", "elapsed=%ld ns", d_ns);
}

static const char *deps_time[] = {NULL};

static run_command_t commands[] = {
    {"hello", run_hello, NULL, "Print greeting"},
    {"time_test", run_time_test, deps_time, "Test timer"},
    {NULL, NULL, NULL, NULL}
};

int run_function(const char *name, int auto_load_deps) {
    if (!name || !*name) {
        uart_puts("Error: empty function name.\n");
        return -1;
    }
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(name, commands[i].name) == 0) {
            if (commands[i].deps && !auto_load_deps) {
                uart_puts("Function '"); uart_puts(name); uart_puts("' has deps: ");
                for (const char **d = commands[i].deps; *d; d++) {
                    uart_puts(*d); uart_puts(" ");
                }
                uart_puts("\nLoad automatically? (y/n) ");
                char c = uart_getc(); uart_putc(c); uart_puts("\r\n");
                if (c != 'y' && c != 'Y') {
                    uart_puts("Aborted.\n");
                    return -1;
                }
                uart_puts("Loading deps...\n");
            }
            uart_puts("Running '"); uart_puts(name); uart_puts("'...\n");
            commands[i].func();
            uart_puts("Function returned.\n");
            return 0;
        }
    }
    uart_puts("Function not found: "); uart_puts(name); uart_puts("\n");
    return -1;
}