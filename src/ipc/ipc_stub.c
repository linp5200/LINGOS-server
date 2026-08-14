/**
 * @file    ipc_stub.c
 * @brief   IPC 通信桩实现（用于无守护进程的简化环境）
 * @version 2.0.0.0
 */

#include "../lib/platform.h"
#include "ipc_stub.h"
#include "log_extra.h"

void ipc_init(void) {
    LOG_INFO_T("IPCStub", "Init", "Stub", "IPC initialized (no-op)");
}

int ipc_send(const char *data, uint32_t len) {
    (void)data;
    (void)len;
    LOG_DEBUG_T("IPCStub", "Send", "Stub", "Dropped %u bytes", len);
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
    LOG_DEBUG_T("IPCStub", "Disconnect", "Stub", "No-op disconnect");
}