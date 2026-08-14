#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H
#include <stdint.h>

void tcp_client_init(void);
int tcp_send_recv(const char *host, uint16_t port,
                  const char *body, char *resp, uint32_t resp_len,
                  int timeout_ms);
#endif