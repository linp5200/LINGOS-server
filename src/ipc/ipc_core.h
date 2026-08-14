#ifndef IPC_IPC_CORE_H
#define IPC_IPC_CORE_H
#include <stdint.h>

void ipc_init(void);
int  ipc_send(const char *data, uint32_t len);
int  ipc_recv(char *buf, uint32_t len);
int  ipc_recv_timeout(char *buf, uint32_t len, int timeout_sec);
void ipc_disconnect(void);

#endif