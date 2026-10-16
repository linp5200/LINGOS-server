/**
 * @file    api_routes.h
 * @brief   API 路由注册头文件
 * @version LN-B-4.3.0.0
 */

#ifndef API_ROUTES_H
#define API_ROUTES_H

/* 【第 4 期】与 src/api/http_server.c 同源：默认走内置 mhd_compat（ENABLE_SYSTEM_MHD=0）
 *   此前本头文件硬编码 <microhttpd.h>，与 http_server.c 的 mhd_compat 类型冲突
 *   （enum MHD_Result 重复定义），导致这些路由函数**无法被任何调用者包含** →
 *   整个 api_routes.c 成为死代码（/api/skills 等路由不存在）。 */
#ifdef LINGOS_USE_SYSTEM_MHD
#include <microhttpd.h>
#else
#include "../net/mhd_compat.h"
#endif

int api_route_alert(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_update(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_rule(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_voice(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_vision(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_app(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_system(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
/* 【第 4 期】补齐头文件缺失声明：api_routes.c 中已实现但此前**未声明**
 *   （http_server.c 因而无法调用 → 模块整体死代码） */
int api_route_ai(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_skills(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_memory(struct MHD_Connection *conn, const char *path, const char *method, const char *body);
int api_route_devices(struct MHD_Connection *conn, const char *path, const char *method, const char *body);

#endif /* API_ROUTES_H */