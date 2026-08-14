#ifndef DEBUG_ERROR_LOGGER_H
#define DEBUG_ERROR_LOGGER_H

#include <stdarg.h>

void log_error_to_file(const char *tag, const char *fmt, ...);
void log_error_to_file_va(const char *tag, const char *fmt, va_list args);

#endif