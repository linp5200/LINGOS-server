#include "../lib/platform.h"
#include "libling.h"
#include "uart.h"

int simple_snprintf(char *buf, int size, const char *fmt, ...) {
    if (!buf || size <= 0) return 0;
    buf[0] = '\0';
    int written = 0;
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    const char *p = fmt;
    while (*p && written < size - 1) {
        if (*p == '%' && *(p + 1) == 's') {
            const char *s = __builtin_va_arg(args, const char*);
            if (s) {
                while (*s && written < size - 1) buf[written++] = *s++;
            }
            p += 2;
        } else {
            buf[written++] = *p++;
        }
    }
    __builtin_va_end(args);
    buf[written] = '\0';
    return written;
}

char *simple_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return NULL;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *simple_strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

void int_to_str(int n, char *buf) {
    int i = 0;
    if (n == 0) buf[i++] = '0';
    else {
        char tmp[8]; int j = 0;
        while (n) { tmp[j++] = '0' + n % 10; n /= 10; }
        while (j) buf[i++] = tmp[--j];
    }
    buf[i] = '\0';
}

void print_hex64(uint64_t val) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nib = (val >> shift) & 0xF;
        uart_putc(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
}