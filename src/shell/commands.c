/**
 * @file    commands.c
 * @brief   调试命令处理（run hello, run time_test 等）
 * @version 2.0.0.0
 */

#include "../lib/platform.h"
#include "commands.h"
#include "../common/string_no_sys.h"
#include "uart.h"
#include "log_extra.h"

static void run_hello(void) {
    uart_puts("Hello from LING OS debug command!\n");
    LOG_INFO_T("DebugCmd", "Hello", "Exec", "hello command executed");
}
static void run_time_test(void) {
    uart_puts("Time function not yet implemented.\n");
}
static const char *deps_time[] = {"timer_init", NULL};

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