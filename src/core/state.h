#ifndef CORE_STATE_H
#define CORE_STATE_H

#include <stdint.h>

typedef struct {
    char id[64];
    char name[128];
    char version[32];
    int enabled;
    int running;
    char description[256];
    uint64_t timestamp;
} component_state_t;

int component_state_init(void);
int component_state_register(const component_state_t *state);
int component_state_set_running(const char *id, int running);
int component_state_get(const char *id, component_state_t *out);
void component_state_scan(void);

#endif