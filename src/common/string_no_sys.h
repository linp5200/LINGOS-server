#ifndef COMMON_STRING_NO_SYS_H
#define COMMON_STRING_NO_SYS_H

#include "../lib/platform.h"

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
int   atoi(const char *str);

#endif