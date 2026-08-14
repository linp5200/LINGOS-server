#ifndef COMMON_MODE_H
#define COMMON_MODE_H

typedef enum {
    MODE_APP,
    MODE_SYSTEM
} lingos_mode_t;

lingos_mode_t lingos_get_mode(void);
int lingos_set_mode(lingos_mode_t mode);
int lingos_mode_config_valid(void);
const char *lingos_mode_name(lingos_mode_t mode);

#endif