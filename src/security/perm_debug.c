#include "../lib/platform.h"
#include "perm_debug.h"
#include "permission.h"
#include "uart.h"
#include "log_extra.h"

void perm_debug_list_apps(void) {
    uart_puts("Apps: 0:System UI 1:Settings 2:Browser 3:Terminal 4:Files\n");
    LOG_DEBUG_T("PermDebug", "List", "Apps", "listed apps");
}
void perm_debug_set_perm(uint32_t id, int p, int m) {
    if (p < 0 || p >= PERM_COUNT || m < 0 || m > 4) {
        uart_puts("Invalid.\n");
        return;
    }
    permission_set_mode(id, (permission_t)p, (auth_mode_t)m);
    uart_puts("Permission updated.\n");
    LOG_DEBUG_T("PermDebug", "Set", "Perm", "app=%d perm=%d mode=%d", id, p, m);
}