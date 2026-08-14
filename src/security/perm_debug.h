#ifndef SECURITY_PERM_DEBUG_H
#define SECURITY_PERM_DEBUG_H
#include "../common/types.h"

void perm_debug_list_apps(void);
void perm_debug_set_perm(uint32_t app_id, int perm, int mode);

#endif