#ifndef SECURITY_INPUT_FILTER_H
#define SECURITY_INPUT_FILTER_H

#include <stdint.h>

int input_filter_check(const char *input, char *reason, uint32_t reason_len);
void input_filter_init(void);
int input_filter_reload(void);

#endif