/**
 * @file    websocket_server.h
 * @brief   WebSocket 服务器头文件
 * @version LN-B-4.3.0.0
 */

#ifndef API_WEBSOCKET_SERVER_H
#define API_WEBSOCKET_SERVER_H

/* 【修复A】WS 端口公开宏——协议 v3 定稿 2939（原 .c 内部 3940 与协议不符） */
#define WEBSOCKET_PORT 2939

int websocket_server_start(void);
void websocket_server_stop(void);
int websocket_broadcast(const char *topic, const char *message);
int websocket_broadcast_all(const char *message);
int websocket_client_count(void);

#endif /* API_WEBSOCKET_SERVER_H */