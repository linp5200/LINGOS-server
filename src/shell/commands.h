#ifndef SHELL_COMMANDS_H
#define SHELL_COMMANDS_H
#include "../common/types.h"

typedef struct {
    const char *name; void (*func)(void); const char **deps; const char *desc;
} run_command_t;

int run_function(const char *name, int auto_load_deps);
#endif