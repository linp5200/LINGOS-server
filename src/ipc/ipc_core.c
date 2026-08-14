/**
 * @file    ipc_core.c
 * @brief   IPC 通信（桩实现，实际由 lingosd 处理）
 * @version 2.0.0.0
 */

#include "../lib/platform.h"
#include "ipc_core.h"
#include "log_extra.h"

void ipc_init(void) {
    LOG_INFO_T("IPC", "Init", "Stub", "IPC initialized (no-op)");
}

int ipc_send(const char *data, uint32_t len) {
    (void)data;
    (void)len;
    LOG_DEBUG_T("IPC", "Send", "Stub", "Dropped %u bytes", len);
    return 0;
}

int ipc_recv(char *buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0;
}

int ipc_recv_timeout(char *buf, uint32_t len, int timeout_sec) {
    (void)buf;
    (void)len;
    (void)timeout_sec;
    return 0;
}

void ipc_disconnect(void) {
    LOG_DEBUG_T("IPC", "Disconnect", "Stub", "No-op disconnect");
}