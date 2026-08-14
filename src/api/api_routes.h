/**
 * @file    api_routes.h
 * @brief   API 路由注册头文件
 * @version LN-B-4.3.0.0
 */

#ifndef API_ROUTES_H
#define API_ROUTES_H

#include <microhttpd.h>

int api_route_alert(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_update(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_rule(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_voice(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_vision(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_app(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_system(struct MHD_Connection *conn, const char *path, const char *method, const char *body);

#endif /* API_ROUTES_H */