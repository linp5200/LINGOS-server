#ifndef SECURITY_OUTPUT_FILTER_H
#define SECURITY_OUTPUT_FILTER_H

#include <stdint.h>

const char *output_filter_redact(const char *output);
int output_filter_reload(void);

#endif