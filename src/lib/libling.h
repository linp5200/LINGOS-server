#ifndef LIB_LIBLING_H
#define LIB_LIBLING_H

#include "platform.h"

int   simple_snprintf(char *buf, int size, const char *fmt, ...);
char *simple_strstr(const char *haystack, const char *needle);
char *simple_strncpy(char *dest, const char *src, size_t n);
void  int_to_str(int n, char *buf);
void  print_hex64(uint64_t val);

#endif