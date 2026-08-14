#ifndef UPDATE_COMPONENT_VERSION_H
#define UPDATE_COMPONENT_VERSION_H

#include <stdint.h>

typedef struct {
    const char *name;
    const char *path;
    const char *cur_version;
    const char *min_supported;
    const char *max_supported;
    int (*migrate)(void);
} component_t;

int component_version_init(void);
const char *component_get_version(const char *name);
int component_is_compatible(const component_t *comp);
int component_upgrade(const char *name);
int component_upgrade_all(void);
void component_show_status(void);
int component_register(component_t *comp);

#endif