#ifndef DEBUG_CRASH_HANDLER_H
#define DEBUG_CRASH_HANDLER_H

#include <signal.h>

void crash_handler_init(void);
void crash_dump_log(const char *reason);

#endif