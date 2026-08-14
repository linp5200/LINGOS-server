/**
 * @file    discovery_server.h
 * @brief   UDP 局域网发现服务头文件 - App 自动发现 LING OS 主机
 * @version LN-B-5.0.0.0
 */

#ifndef DAEMON_DISCOVERY_SERVER_H
#define DAEMON_DISCOVERY_SERVER_H

int discovery_server_start(void);
void discovery_server_stop(void);
int discovery_server_is_running(void);

#endif /* DAEMON_DISCOVERY_SERVER_H */
