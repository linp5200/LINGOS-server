#ifndef API_HTTP_SERVER_H
#define API_HTTP_SERVER_H

/* 启动 HTTP API 服务器（后台线程）*/
int http_server_start(int port);

/* 停止 HTTP API 服务器 */
void http_server_stop(void);

/* 检查是否运行中 */
int http_server_is_running(void);

#endif