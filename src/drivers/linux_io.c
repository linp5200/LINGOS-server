#include "../lib/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include "linux_io.h"
#include "log_extra.h"

static struct termios old_tio;

void linux_io_init(void) {
    LOG_INFO_T("LinuxIO", "Init", "Console", "Console I/O ready (non‑canonical mode)");
    struct termios new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void uart_puts(const char *str) {
    printf("%s", str);
    fflush(stdout);
}

void uart_putc(char c) {
    putchar(c);
    fflush(stdout);
}

char uart_getc(void) {
    int ch = getchar();
    if (ch == EOF) return 0;
    return (char)ch;
}

void uart1_puts(const char *str) { uart_puts(str); }
void uart1_putc(char c)          { uart_putc(c); }
char uart1_getc(void)            { return uart_getc(); }