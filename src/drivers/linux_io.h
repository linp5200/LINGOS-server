#ifndef LINUX_IO_H
#define LINUX_IO_H

void linux_io_init(void);
void uart_puts(const char *str);
void uart_putc(char c);
char uart_getc(void);
void uart1_puts(const char *str);
void uart1_putc(char c);
char uart1_getc(void);

#endif