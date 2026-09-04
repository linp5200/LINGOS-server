/**
 * @file    port_config.h
 * @brief   端口配置接口（2026-08-22 先生裁决：port 指令族）
 * @version LN-0.4.3
 */

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define WEBSOCKET_PORT_DEFAULT 2939
#define HTTP_PORT_DEFAULT      8080
#define TCP_PORT_DEFAULT       2937

typedef enum {
    PORT_WS = 0,
    PORT_HTTP = 1,
    PORT_TCP = 2
} port_type_t;

/* 获取端口（启动时从 ports.json 读取，缺省用宏默认） */
int port_config_get(port_type_t type);

/* 设置端口（写 ports.json，重启生效）；非法端口返回 -1 */
int port_config_set(port_type_t type, int port);

/* 端口名（"ws"/"http"/"tcp"） */
const char* port_config_name(port_type_t type);

#endif /* PORT_CONFIG_H */
