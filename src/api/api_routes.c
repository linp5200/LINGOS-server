/**
 * @file    src/api/api_routes.c
 * @brief   API 路由注册 - 完整版（含系统状态、AI对话、技能执行、记忆操作）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 扩展所有模块端点；增加 POST 支持；集成 AI 和系统功能
 */

#include "api_routes.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../health/system_health.h"
#include "../ai/nook.h"
#include "../ai/ai_config.h"
#include "../ai/ai_privilege.h"
#include "../config/config_core.h"
#include "../core/version.h"
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* ============================================================
 * 内部工具：发送 JSON 响应
 * ============================================================ */
static void send_json_response(struct MHD_Connection *conn, int code, const char *json) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(json), (void*)json, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_queue_response(conn, code, resp);
    MHD_destroy_response(resp);
}

static void send_error(struct MHD_Connection *conn, int code, const char *msg) {
    char buf[256];
    safe_snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    send_json_response(conn, code, buf);
}

static void send_ok(struct MHD_Connection *conn, const char *data) {
    char buf[4096];
    safe_snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"data\":%s}", data ? data : "null");
    send_json_response(conn, MHD_HTTP_OK, buf);
}

/* ============================================================
 * 系统模块
 * ============================================================ */
int api_route_system(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    (void)body;

    if (strcmp(method, "GET") != 0) {
        send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
        return MHD_YES;
    }

    if (strcmp(path, "status") == 0) {
        const char *version = version_get();
        const wizard_config_t *cfg = config_core_get();

        /* FF[src/health/system_health.c]-CFN[get_memory_usage]-FTF[获取内存使用率] */
        int mem = get_memory_usage();
        /* FF[src/health/system_health.c]-CFN[get_disk_usage]-FTF[获取磁盘使用率] */
        int disk = get_disk_usage(lingos_data_root());
        double load1, load5, load15;
        get_load_avg(&load1, &load5, &load15);

        char buf[1024];
        safe_snprintf(buf, sizeof(buf),
            "{\"version\":\"%s\",\"mode\":\"%s\",\"uptime\":%ld,"
            "\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,"
            "\"ai_backend\":\"%s\",\"language\":\"%s\"}",
            version ? version : "unknown",
            "app",
            (long)time(NULL),
            mem, disk, load1,
            cfg && cfg->ai_backend[0] ? cfg->ai_backend : "unknown",
            cfg && cfg->language[0] ? cfg->language : "en"
        );
        send_ok(conn, buf);
        return MHD_YES;
    }

    if (strcmp(path, "metrics") == 0) {
        char buf[512];
        int mem = get_memory_usage();
        int disk = get_disk_usage(lingos_data_root());
        double load1, load5, load15;
        get_load_avg(&load1, &load5, &load15);

        safe_snprintf(buf, sizeof(buf),
            "{\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,\"cpu_cores\":%ld}",
            mem, disk, load1, sysconf(_SC_NPROCESSORS_ONLN)
        );
        send_ok(conn, buf);
        return MHD_YES;
    }

    if (strcmp(path, "health") == 0) {
        char buf[512];
        int mem = get_memory_usage();
        int disk = get_disk_usage(lingos_data_root());
        double load1, load5, load15;
        get_load_avg(&load1, &load5, &load15);

        safe_snprintf(buf, sizeof(buf),
            "{\"status\":\"ok\",\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,\"python\":%d,\"ai\":%d,\"network\":%d}",
            mem, disk, load1, check_python(), check_ai_backend(), check_network()
        );
        send_json_response(conn, MHD_HTTP_OK, buf);
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "system endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * AI 模块
 * ============================================================ */
int api_route_ai(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    if (strcmp(path, "ask") == 0) {
        if (strcmp(method, "POST") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }

        /* 解析请求体（简化 JSON 解析） */
        if (!body) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing body");
            return MHD_YES;
        }

        char prompt[512] = {0};
        char session_id[64] = "default";
        int timeout = 60;

        /* 简单 JSON 提取 */
        char *p = strstr(body, "\"prompt\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        int len = end - p;
                        if (len < (int)sizeof(prompt) - 1) {
                            strncpy(prompt, p, len);
                            prompt[len] = '\0';
                        }
                    }
                }
            }
        }

        if (!prompt[0]) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing prompt");
            return MHD_YES;
        }

        p = strstr(body, "\"session_id\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        int len = end - p;
                        if (len < (int)sizeof(session_id) - 1) {
                            strncpy(session_id, p, len);
                            session_id[len] = '\0';
                        }
                    }
                }
            }
        }

        /* FF[src/ai/nook.c]-CFN[nook_ask_ollama]-FTF[向 AI 发送对话请求] */
        char response[8192];
        int ret = nook_ask_ollama(prompt, NULL, response, sizeof(response), timeout);

        if (ret == 0) {
            char buf[8448];
            safe_snprintf(buf, sizeof(buf), "{\"response\":\"%s\",\"session_id\":\"%s\"}",
                          response, session_id);
            send_ok(conn, buf);
        } else if (ret == -2) {
            send_error(conn, MHD_HTTP_GATEWAY_TIMEOUT, "AI request timed out");
        } else {
            send_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, "AI service unavailable");
        }
        return MHD_YES;
    }

    if (strcmp(path, "status") == 0) {
        if (strcmp(method, "GET") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }

        const ai_config_t *cfg = ai_config_get();
        char buf[256];
        safe_snprintf(buf, sizeof(buf),
            "{\"available\":%d,\"backend\":\"%s\",\"model\":\"%s\",\"language\":\"%s\"}",
            cfg ? 1 : 0,
            cfg && cfg->backend == AI_BACKEND_OLLAMA ? "ollama" : "deepseek",
            cfg ? cfg->deepseek_model : "unknown",
            cfg ? cfg->language : "en"
        );
        send_ok(conn, buf);
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "ai endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * 技能模块
 * ============================================================ */
int api_route_skills(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    if (strcmp(path, "list") == 0) {
        if (strcmp(method, "GET") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }

        /* 返回默认技能列表（简版） */
        send_ok(conn, "[\"file_read\",\"file_write\",\"file_delete\",\"file_list\","
                       "\"system_info\",\"system_memory\",\"system_disk\",\"system_cpu\","
                       "\"net_ping\",\"net_status\",\"process_list\",\"package_list\","
                       "\"memory_read\",\"memory_write\",\"memory_search\"]");
        return MHD_YES;
    }

    if (strcmp(path, "run") == 0) {
        if (strcmp(method, "POST") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }

        if (!body) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing body");
            return MHD_YES;
        }

        /* 提取技能名称和参数 */
        char skill_name[64] = {0};
        char args_json[512] = "{}";
        char *p = strstr(body, "\"skill\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        int len = end - p;
                        if (len < (int)sizeof(skill_name) - 1) {
                            strncpy(skill_name, p, len);
                            skill_name[len] = '\0';
                        }
                    }
                }
            }
        }

        if (!skill_name[0]) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing skill name");
            return MHD_YES;
        }

        /* 权限检查（L2+ 需要授权） */
        /* FF[src/ai/ai_privilege.c]-CFN[ai_privilege_is_high_risk]-FTF[检查是否为高风险技能] */
        int high_risk = ai_privilege_is_high_risk(skill_name);
        if (high_risk) {
            /* 检查是否有授权缓存 */
            /* FF[src/ai/ai_privilege.c]-CFN[ai_privilege_check]-FTF[检查权限是否已授权] */
            if (!ai_privilege_check(skill_name)) {
                send_error(conn, MHD_HTTP_FORBIDDEN, "skill requires authorization");
                return MHD_YES;
            }
        }

        /* 执行技能（通过 nook 间接调用） */
        char request[512];
        safe_snprintf(request, sizeof(request), "执行技能 %s，参数：%s", skill_name, args_json);
        char response[4096];
        int ret = nook_ask_ollama(request, NULL, response, sizeof(response), 60);

        if (ret == 0) {
            char buf[4224];
            safe_snprintf(buf, sizeof(buf), "{\"skill\":\"%s\",\"result\":\"%s\"}", skill_name, response);
            send_ok(conn, buf);
        } else {
            send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "skill execution failed");
        }
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "skills endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * 记忆模块
 * ============================================================ */
int api_route_memory(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    (void)body;

    if (strcmp(method, "POST") != 0) {
        send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
        return MHD_YES;
    }

    if (strcmp(path, "write") == 0) {
        if (!body) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing body");
            return MHD_YES;
        }

        /* 提取内容和关键词 */
        char content[256] = {0};
        char *p = strstr(body, "\"content\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        int len = end - p;
                        if (len < (int)sizeof(content) - 1) {
                            strncpy(content, p, len);
                            content[len] = '\0';
                        }
                    }
                }
            }
        }

        if (!content[0]) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing content");
            return MHD_YES;
        }

        /* 通过 AI 写入记忆（模拟） */
        char request[512];
        safe_snprintf(request, sizeof(request), "记住：%s", content);
        char response[4096];
        int ret = nook_ask_ollama(request, NULL, response, sizeof(response), 30);

        if (ret == 0) {
            char buf[512];
            safe_snprintf(buf, sizeof(buf), "{\"content\":\"%s\",\"status\":\"saved\"}", content);
            send_ok(conn, buf);
        } else {
            send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "failed to save memory");
        }
        return MHD_YES;
    }

    if (strcmp(path, "search") == 0) {
        if (!body) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing body");
            return MHD_YES;
        }

        char keyword[128] = {0};
        char *p = strstr(body, "\"keyword\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        int len = end - p;
                        if (len < (int)sizeof(keyword) - 1) {
                            strncpy(keyword, p, len);
                            keyword[len] = '\0';
                        }
                    }
                }
            }
        }

        if (!keyword[0]) {
            send_error(conn, MHD_HTTP_BAD_REQUEST, "missing keyword");
            return MHD_YES;
        }

        char request[512];
        safe_snprintf(request, sizeof(request), "搜索记忆：%s", keyword);
        char response[4096];
        int ret = nook_ask_ollama(request, NULL, response, sizeof(response), 30);

        if (ret == 0) {
            char buf[4224];
            safe_snprintf(buf, sizeof(buf), "{\"keyword\":\"%s\",\"results\":\"%s\"}", keyword, response);
            send_ok(conn, buf);
        } else {
            send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "search failed");
        }
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "memory endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * 预警模块
 * ============================================================ */
int api_route_alert(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    (void)body;

    if (strcmp(method, "GET") != 0) {
        send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
        return MHD_YES;
    }

    if (strcmp(path, "status") == 0) {
        send_ok(conn, "{\"alerts\":[],\"count\":0}");
        return MHD_YES;
    }

    if (strcmp(path, "history") == 0) {
        send_ok(conn, "{\"history\":[],\"total\":0}");
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "alert endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * 设备模块
 * ============================================================ */
int api_route_devices(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    (void)body;

    if (strcmp(path, "list") == 0) {
        if (strcmp(method, "GET") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }
        send_ok(conn, "{\"devices\":[],\"total\":0}");
        return MHD_YES;
    }

    if (strcmp(path, "control") == 0) {
        if (strcmp(method, "POST") != 0) {
            send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
            return MHD_YES;
        }
        send_ok(conn, "{\"status\":\"not_implemented\"}");
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "devices endpoint not found");
    return MHD_YES;
}

/* ============================================================
 * 更新模块（占位）
 * ============================================================ */
int api_route_update(struct MHD_Connection *conn, const char *path, const char *method, const char *body) {
    (void)body;

    if (strcmp(method, "GET") != 0) {
        send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");
        return MHD_YES;
    }

    if (strcmp(path, "check") == 0) {
        send_ok(conn, "{\"update_available\":false,\"latest\":\"LN-0.4.3\"}");
        return MHD_YES;
    }

    send_error(conn, MHD_HTTP_NOT_FOUND, "update endpoint not found");
    return MHD_YES;
}